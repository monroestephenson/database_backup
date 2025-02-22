#include "backup_manager.hpp"
#include "db_connection.hpp"
#include "compression.hpp"
#include "storage.hpp"
#include "logging.hpp"
#include "notifications.hpp"
#include "error/ErrorUtils.hpp"

#include <iostream>
#include <chrono>
#include <ctime>
#include <filesystem>

using namespace dbbackup::error;  // Add this line to bring error types into scope

BackupManager::BackupManager(const dbbackup::Config& cfg)
    : m_config(cfg)
{
    // Validate configuration in constructor
    if (cfg.database.type.empty()) {
        DB_THROW(ConfigurationError, "Database type not specified");
    }
    if (cfg.storage.localPath.empty()) {
        DB_THROW(ConfigurationError, "Storage path not specified");
    }
    if (cfg.logging.logPath.empty()) {
        DB_THROW(ConfigurationError, "Log path not specified");
    }
    if (cfg.logging.logLevel.empty()) {
        DB_THROW(ConfigurationError, "Log level not specified");
    }
}

BackupManager::~BackupManager() = default;

bool BackupManager::backup(const std::string& backupType) {
    // Create compressor outside the macro if compression is enabled
    std::unique_ptr<dbbackup::Compressor> compressor;
    if (m_config.backup.compression.enabled) {
        compressor = std::make_unique<dbbackup::Compressor>(m_config.backup.compression);
    }

    DB_TRY_CATCH_LOG("BackupManager", {
        // Validate input
        DB_CHECK(!backupType.empty(), ValidationError, "Backup type cannot be empty");
        DB_CHECK(backupType == "full" || backupType == "incremental" || backupType == "differential",
                ValidationError, "Invalid backup type: " + backupType);

        // Get logger
        auto logger = getLogger();
        logger->info("Starting {} backup...", backupType);

        // Create and validate connection
        auto conn = createConnection();
        DB_CHECK(conn != nullptr, ConnectionError, "Failed to create database connection");

        // Connect to database
        if (!conn->connect(m_config.database)) {
            DB_THROW(ConnectionError, "Failed to connect to database");
        }

        // Create backup directory if it doesn't exist
        std::filesystem::path backupDir(m_config.storage.localPath);
        if (!std::filesystem::exists(backupDir)) {
            DB_CHECK(std::filesystem::create_directories(backupDir),
                    StorageError, "Failed to create backup directory: " + m_config.storage.localPath);
        }
        
        // Prepare backup file path
        auto t = std::time(nullptr);
        auto tm = *std::localtime(&t);

        char timeBuf[128];
        strftime(timeBuf, sizeof(timeBuf), "%Y%m%d_%H%M%S", &tm);
        std::string backupFileName = "backup_" + std::string(timeBuf) + "_" + backupType;
        
        // Create temporary file path with unique name
        std::string tempPath = m_config.storage.localPath + "/.tmp_" + backupFileName + ".dump";
        
        // Final backup path
        std::string finalPath = m_config.storage.localPath + "/" + backupFileName + 
                               (m_config.backup.compression.enabled ? ".dump.gz" : ".dump");

        // Remove any existing temporary files
        if (std::filesystem::exists(tempPath)) {
            std::filesystem::remove(tempPath);
        }

        // Perform backup to temporary file
        if (!conn->createBackup(tempPath)) {
            DB_THROW(BackupError, "Failed to create backup at: " + tempPath);
        }

        bool success = false;
        try {
            if (compressor) {
                // Try to compress directly to final location
                success = compressor->compressFile(tempPath, finalPath);
                if (!success) {
                    DB_THROW(CompressionError, "Failed to compress backup file");
                }
            } else {
                // Move uncompressed file to final location
                std::filesystem::rename(tempPath, finalPath);
                success = true;
            }
        } catch (const std::exception& e) {
            // Clean up temporary file
            if (std::filesystem::exists(tempPath)) {
                std::filesystem::remove(tempPath);
            }
            DB_THROW(BackupError, std::string("Backup failed: ") + e.what());
        }

        // Clean up temporary file
        if (std::filesystem::exists(tempPath)) {
            std::filesystem::remove(tempPath);
        }

        // Verify backup exists
        if (!std::filesystem::exists(finalPath)) {
            DB_THROW(StorageError, "Backup file not found after creation: " + finalPath);
        }

        // Disconnect database
        if (!conn->disconnect()) {
            logger->warn("Failed to disconnect from database");
        }

        // Log success and send notification if enabled
        logger->info("Backup completed successfully: {}", finalPath);
        if (m_config.logging.enableNotifications) {
            sendNotificationIfNeeded(m_config.logging, "Backup succeeded: " + finalPath);
        }

        return true;
    });

    return false; // Only reached if an exception was caught
}

bool BackupManager::restore(const std::string& backupPath) {
    // Create compressor outside the macro if compression is enabled
    std::unique_ptr<dbbackup::Compressor> compressor;
    if (m_config.backup.compression.enabled) {
        compressor = std::make_unique<dbbackup::Compressor>(m_config.backup.compression);
    }

    DB_TRY_CATCH_LOG("BackupManager", {
        // Validate input
        DB_CHECK(!backupPath.empty(), ValidationError, "Backup path cannot be empty");
        DB_CHECK(std::filesystem::exists(backupPath), ValidationError, "Backup file not found: " + backupPath);

        // Get logger
        auto logger = getLogger();
        logger->info("Starting restore from {}...", backupPath);

        // Create and validate connection
        auto conn = createConnection();
        DB_CHECK(conn != nullptr, ConnectionError, "Failed to create database connection");

        // Connect to database
        if (!conn->connect(m_config.database)) {
            DB_THROW(ConnectionError, "Failed to connect to database");
        }

        // Decompress if needed
        std::string restorePath = backupPath;
        if (compressor) {
            std::string uncompressedPath = backupPath;
            size_t extPos = backupPath.find(compressor->getFileExtension());
            if (extPos != std::string::npos) {
                uncompressedPath = backupPath.substr(0, extPos);
                if (!compressor->decompressFile(backupPath, uncompressedPath)) {
                    DB_THROW(CompressionError, "Failed to decompress backup file");
                }
                restorePath = uncompressedPath;
            }
        }

        // Perform restore
        if (!conn->restoreBackup(restorePath)) {
            DB_THROW(RestoreError, "Failed to restore from backup: " + restorePath);
        }

        // Clean up decompressed file if we created one
        if (restorePath != backupPath) {
            if (!std::filesystem::remove(restorePath)) {
                logger->warn("Failed to remove temporary decompressed file: {}", restorePath);
            }
        }

        // Disconnect database
        if (!conn->disconnect()) {
            logger->warn("Failed to disconnect from database");
        }

        // Log success and send notification
        logger->info("Restore completed successfully from: {}", backupPath);
        sendNotificationIfNeeded(m_config.logging, "Restore succeeded from: " + backupPath);

        return true;
    });

    return false; // Only reached if an exception was caught
}

std::unique_ptr<IDBConnection> BackupManager::createConnection() {
    DB_TRY_CATCH_LOG("BackupManager", {
        // Validate database configuration
        if (m_config.database.type.empty()) {
            DB_THROW(ConfigurationError, "Database type not specified");
        }
        
        // For SQLite, we only need the database path
        if (m_config.database.type == "sqlite") {
            if (m_config.database.database.empty()) {
                DB_THROW(ConfigurationError, "SQLite database file path not specified");
            }
        } else {
            // For other databases, we need host
            if (m_config.database.host.empty()) {
                DB_THROW(ConfigurationError, "Database host not specified");
            }
        }
        
        return createDBConnection(m_config.database);
    });
    
    return nullptr; // Only reached if an exception was caught
}

