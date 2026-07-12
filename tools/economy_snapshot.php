<?php
declare(strict_types=1);

date_default_timezone_set('Asia/Bangkok');

$root = realpath(__DIR__ . '/..') ?: dirname(__DIR__);
$interConf = $root . '/conf/inter_athena.conf';
$schemaFile = __DIR__ . '/economy_snapshot.sql';

function parse_conf(string $file): array
{
    $data = [];
    foreach (file($file, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) ?: [] as $line) {
        $line = preg_replace('/\/\/.*$/', '', $line);
        if (!is_string($line) || !str_contains($line, ':')) {
            continue;
        }
        [$key, $value] = explode(':', $line, 2);
        $data[trim($key)] = trim($value);
    }
    return $data;
}

function db_connect(array $conf, string $prefix): mysqli
{
    mysqli_report(MYSQLI_REPORT_ERROR | MYSQLI_REPORT_STRICT);
    $db = new mysqli(
        $conf[$prefix . '_ip'] ?? '127.0.0.1',
        $conf[$prefix . '_id'] ?? '',
        $conf[$prefix . '_pw'] ?? '',
        $conf[$prefix . '_db'] ?? '',
        (int)($conf[$prefix . '_port'] ?? 3306)
    );
    $db->set_charset('utf8mb4');
    return $db;
}

function scalar(mysqli $db, string $sql): int
{
    $row = $db->query($sql)->fetch_row();
    return (int)($row[0] ?? 0);
}

function rows(mysqli $db, string $sql): array
{
    $result = $db->query($sql);
    $rows = [];
    while ($row = $result->fetch_assoc()) {
        $rows[] = $row;
    }
    return $rows;
}

function exec_schema(mysqli $db, string $schemaFile): void
{
    $sql = (string)file_get_contents($schemaFile);
    foreach (array_filter(array_map('trim', explode(';', $sql))) as $stmt) {
        if ($stmt !== '') {
            $db->query($stmt);
        }
    }
}

function insert_supply(mysqli $db, int $snapshotId, string $source, array $rows): void
{
    $stmt = $db->prepare(
        'INSERT INTO admin_item_supply_snapshot (snapshot_id, source, nameid, amount, stacks) VALUES (?, ?, ?, ?, ?)'
    );
    foreach ($rows as $row) {
        $nameid = (int)$row['nameid'];
        $amount = (int)$row['amount'];
        $stacks = (int)$row['stacks'];
        $stmt->bind_param('isiii', $snapshotId, $source, $nameid, $amount, $stacks);
        $stmt->execute();
    }
    $stmt->close();
}

function insert_alert(mysqli $db, int $snapshotId, string $createdAt, string $severity, string $metric, string $message, int $now, int $prev): void
{
    $delta = $now - $prev;
    $stmt = $db->prepare(
        'INSERT INTO admin_economy_alert (snapshot_id, created_at, severity, metric, message, value_now, value_prev, delta_value)
         VALUES (?, ?, ?, ?, ?, ?, ?, ?)'
    );
    $stmt->bind_param('issssiii', $snapshotId, $createdAt, $severity, $metric, $message, $now, $prev, $delta);
    $stmt->execute();
    $stmt->close();
}

function previous_snapshot(mysqli $db, int $snapshotId): ?array
{
    $result = $db->query(
        'SELECT * FROM admin_economy_snapshot WHERE id < ' . (int)$snapshotId . ' ORDER BY id DESC LIMIT 1'
    );
    $row = $result->fetch_assoc();
    return $row ?: null;
}

function create_summary_alerts(mysqli $db, int $snapshotId, string $createdAt, array $current, ?array $previous): void
{
    if ($previous === null) {
        insert_alert($db, $snapshotId, $createdAt, 'info', 'snapshot', 'First economy snapshot created', (int)$current['total_zeny'], 0);
        return;
    }

    $checks = [
        ['total_zeny', 10000000, 50000000, 'Total zeny changed quickly'],
        ['picklog_rows', 500, 3000, 'Picklog rows increased quickly'],
        ['cashlog_rows', 50, 300, 'Cashlog rows increased quickly'],
        ['payment_rows', 20, 100, 'Payment rows increased quickly'],
        ['global_drop_rows', 20, 100, 'Global drop rows increased quickly'],
    ];

    foreach ($checks as [$field, $warnDelta, $criticalDelta, $message]) {
        $now = (int)$current[$field];
        $prev = (int)$previous[$field];
        $delta = abs($now - $prev);
        if ($delta >= $criticalDelta) {
            insert_alert($db, $snapshotId, $createdAt, 'critical', $field, $message, $now, $prev);
        } elseif ($delta >= $warnDelta) {
            insert_alert($db, $snapshotId, $createdAt, 'warn', $field, $message, $now, $prev);
        }
    }
}

function create_supply_alerts(mysqli $db, int $snapshotId, string $createdAt): void
{
    $previousId = (int)($db->query(
        'SELECT id FROM admin_economy_snapshot WHERE id < ' . (int)$snapshotId . ' ORDER BY id DESC LIMIT 1'
    )->fetch_row()[0] ?? 0);

    if ($previousId <= 0) {
        return;
    }

    $sql = "
        SELECT cur.source, cur.nameid, cur.amount now_amount, COALESCE(prev.amount, 0) prev_amount
        FROM admin_item_supply_snapshot cur
        LEFT JOIN admin_item_supply_snapshot prev
          ON prev.snapshot_id = {$previousId}
         AND prev.source = cur.source
         AND prev.nameid = cur.nameid
        WHERE cur.snapshot_id = {$snapshotId}
        ORDER BY ABS(cur.amount - COALESCE(prev.amount, 0)) DESC
        LIMIT 20";

    foreach (rows($db, $sql) as $row) {
        $now = (int)$row['now_amount'];
        $prev = (int)$row['prev_amount'];
        $delta = abs($now - $prev);
        if ($delta >= 5000) {
            insert_alert($db, $snapshotId, $createdAt, 'critical', 'item_supply', "Item {$row['nameid']} supply changed in {$row['source']}", $now, $prev);
        } elseif ($delta >= 1000) {
            insert_alert($db, $snapshotId, $createdAt, 'warn', 'item_supply', "Item {$row['nameid']} supply changed in {$row['source']}", $now, $prev);
        }
    }
}

$conf = parse_conf($interConf);
$ragDbName = $conf['char_server_db'] ?? $conf['map_server_db'] ?? '';
$logDbName = $conf['log_db_db'] ?? '';
$rag = db_connect($conf, 'char_server');

exec_schema($rag, $schemaFile);

$now = date('Y-m-d H:i:s');
$summary = rows($rag, "SELECT COALESCE(SUM(zeny),0) total_zeny, COALESCE(AVG(zeny),0) avg_zeny, COALESCE(MAX(zeny),0) max_zeny FROM `{$ragDbName}`.`char`")[0];

$stmt = $rag->prepare(
    'INSERT INTO admin_economy_snapshot
    (created_at, total_zeny, avg_zeny, max_zeny, online_chars, total_chars, inventory_rows, storage_rows, cart_rows, picklog_rows, cashlog_rows, payment_rows, global_drop_rows)
    VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)'
);

$totalZeny = (int)$summary['total_zeny'];
$avgZeny = (int)$summary['avg_zeny'];
$maxZeny = (int)$summary['max_zeny'];
$onlineChars = scalar($rag, "SELECT COUNT(*) FROM `{$ragDbName}`.`char` WHERE online=1");
$totalChars = scalar($rag, "SELECT COUNT(*) FROM `{$ragDbName}`.`char`");
$inventoryRows = scalar($rag, "SELECT COUNT(*) FROM `{$ragDbName}`.`inventory`");
$storageRows = scalar($rag, "SELECT COUNT(*) FROM `{$ragDbName}`.`storage`");
$cartRows = scalar($rag, "SELECT COUNT(*) FROM `{$ragDbName}`.`cart_inventory`");
$picklogRows = scalar($rag, "SELECT COUNT(*) FROM `{$logDbName}`.`picklog`");
$cashlogRows = scalar($rag, "SELECT COUNT(*) FROM `{$logDbName}`.`cashlog`");
$paymentRows = scalar($rag, "SELECT COUNT(*) FROM `{$ragDbName}`.`payment`");
$globalDropRows = scalar($rag, "SELECT COUNT(*) FROM `{$ragDbName}`.`global_drop`");

$stmt->bind_param(
    'siiiiiiiiiiii',
    $now,
    $totalZeny,
    $avgZeny,
    $maxZeny,
    $onlineChars,
    $totalChars,
    $inventoryRows,
    $storageRows,
    $cartRows,
    $picklogRows,
    $cashlogRows,
    $paymentRows,
    $globalDropRows
);
$stmt->execute();
$snapshotId = (int)$rag->insert_id;
$stmt->close();

$currentSnapshot = [
    'total_zeny' => $totalZeny,
    'picklog_rows' => $picklogRows,
    'cashlog_rows' => $cashlogRows,
    'payment_rows' => $paymentRows,
    'global_drop_rows' => $globalDropRows,
];

insert_supply($rag, $snapshotId, 'inventory', rows($rag, "SELECT nameid, COALESCE(SUM(amount),0) amount, COUNT(*) stacks FROM `{$ragDbName}`.`inventory` GROUP BY nameid ORDER BY amount DESC LIMIT 50"));
insert_supply($rag, $snapshotId, 'storage', rows($rag, "SELECT nameid, COALESCE(SUM(amount),0) amount, COUNT(*) stacks FROM `{$ragDbName}`.`storage` GROUP BY nameid ORDER BY amount DESC LIMIT 50"));
insert_supply($rag, $snapshotId, 'cart', rows($rag, "SELECT nameid, COALESCE(SUM(amount),0) amount, COUNT(*) stacks FROM `{$ragDbName}`.`cart_inventory` GROUP BY nameid ORDER BY amount DESC LIMIT 50"));

$flowStmt = $rag->prepare(
    'INSERT INTO admin_item_flow_snapshot (snapshot_id, nameid, type, amount, rows_count) VALUES (?, ?, ?, ?, ?)'
);
foreach (rows($rag, "SELECT nameid, type, COALESCE(SUM(amount),0) amount, COUNT(*) rows_count FROM `{$logDbName}`.`picklog` WHERE time >= DATE_SUB(NOW(), INTERVAL 10 MINUTE) GROUP BY nameid,type ORDER BY ABS(amount) DESC, rows_count DESC LIMIT 50") as $row) {
    $nameid = (int)$row['nameid'];
    $type = (string)$row['type'];
    $amount = (int)$row['amount'];
    $rowsCount = (int)$row['rows_count'];
    $flowStmt->bind_param('iisii', $snapshotId, $nameid, $type, $amount, $rowsCount);
    $flowStmt->execute();
}
$flowStmt->close();

create_summary_alerts($rag, $snapshotId, $now, $currentSnapshot, previous_snapshot($rag, $snapshotId));
create_supply_alerts($rag, $snapshotId, $now);

$rag->query('DELETE FROM admin_economy_snapshot WHERE created_at < DATE_SUB(NOW(), INTERVAL 14 DAY)');
$rag->query('DELETE FROM admin_economy_alert WHERE created_at < DATE_SUB(NOW(), INTERVAL 14 DAY)');
$rag->close();

echo "snapshot_id={$snapshotId} created_at={$now} total_zeny={$totalZeny}\n";
