CREATE TABLE IF NOT EXISTS `admin_economy_snapshot` (
  `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `created_at` DATETIME NOT NULL,
  `total_zeny` BIGINT NOT NULL DEFAULT 0,
  `avg_zeny` BIGINT NOT NULL DEFAULT 0,
  `max_zeny` BIGINT NOT NULL DEFAULT 0,
  `online_chars` INT NOT NULL DEFAULT 0,
  `total_chars` INT NOT NULL DEFAULT 0,
  `inventory_rows` INT NOT NULL DEFAULT 0,
  `storage_rows` INT NOT NULL DEFAULT 0,
  `cart_rows` INT NOT NULL DEFAULT 0,
  `picklog_rows` INT NOT NULL DEFAULT 0,
  `cashlog_rows` INT NOT NULL DEFAULT 0,
  `payment_rows` INT NOT NULL DEFAULT 0,
  `global_drop_rows` INT NOT NULL DEFAULT 0,
  PRIMARY KEY (`id`),
  KEY `created_at` (`created_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;

CREATE TABLE IF NOT EXISTS `admin_item_supply_snapshot` (
  `snapshot_id` BIGINT UNSIGNED NOT NULL,
  `source` ENUM('inventory','storage','cart') NOT NULL,
  `nameid` INT NOT NULL,
  `amount` BIGINT NOT NULL DEFAULT 0,
  `stacks` INT NOT NULL DEFAULT 0,
  PRIMARY KEY (`snapshot_id`, `source`, `nameid`),
  KEY `source_nameid` (`source`, `nameid`),
  CONSTRAINT `admin_item_supply_snapshot_fk`
    FOREIGN KEY (`snapshot_id`) REFERENCES `admin_economy_snapshot` (`id`)
    ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;

CREATE TABLE IF NOT EXISTS `admin_item_flow_snapshot` (
  `snapshot_id` BIGINT UNSIGNED NOT NULL,
  `nameid` INT NOT NULL,
  `type` VARCHAR(16) NOT NULL,
  `amount` BIGINT NOT NULL DEFAULT 0,
  `rows_count` INT NOT NULL DEFAULT 0,
  PRIMARY KEY (`snapshot_id`, `nameid`, `type`),
  KEY `nameid_type` (`nameid`, `type`),
  CONSTRAINT `admin_item_flow_snapshot_fk`
    FOREIGN KEY (`snapshot_id`) REFERENCES `admin_economy_snapshot` (`id`)
    ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;

CREATE TABLE IF NOT EXISTS `admin_economy_alert` (
  `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `snapshot_id` BIGINT UNSIGNED NOT NULL,
  `created_at` DATETIME NOT NULL,
  `severity` ENUM('info','warn','critical') NOT NULL DEFAULT 'info',
  `metric` VARCHAR(64) NOT NULL,
  `message` VARCHAR(255) NOT NULL,
  `value_now` BIGINT NOT NULL DEFAULT 0,
  `value_prev` BIGINT NOT NULL DEFAULT 0,
  `delta_value` BIGINT NOT NULL DEFAULT 0,
  PRIMARY KEY (`id`),
  KEY `created_at` (`created_at`),
  KEY `severity_created_at` (`severity`, `created_at`),
  KEY `snapshot_id` (`snapshot_id`),
  CONSTRAINT `admin_economy_alert_fk`
    FOREIGN KEY (`snapshot_id`) REFERENCES `admin_economy_snapshot` (`id`)
    ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;
