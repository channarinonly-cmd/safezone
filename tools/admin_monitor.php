<?php
declare(strict_types=1);

ini_set('display_errors', '0');
date_default_timezone_set('Asia/Bangkok');

$root = realpath(__DIR__ . '/..') ?: dirname(__DIR__);
$keyFile = $root . '/conf/admin_monitor.key';
$interConf = $root . '/conf/inter_athena.conf';

function fail(int $code, string $message): never
{
    http_response_code($code);
    header('Content-Type: text/plain; charset=utf-8');
    echo $message;
    exit;
}

function read_key(string $keyFile): string
{
    if (!is_file($keyFile)) {
        fail(500, 'admin_monitor.key is missing');
    }
    $key = trim((string)file_get_contents($keyFile));
    if ($key === '') {
        fail(500, 'admin_monitor.key is empty');
    }
    return $key;
}

function require_key(string $keyFile): void
{
    $expected = read_key($keyFile);
    $given = (string)($_GET['key'] ?? $_POST['key'] ?? '');
    if (!hash_equals($expected, $given)) {
        fail(403, 'Forbidden');
    }
}

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

function scalar(mysqli $db, string $sql, string $types = '', mixed ...$params): mixed
{
    $stmt = $db->prepare($sql);
    if ($types !== '') {
        $stmt->bind_param($types, ...$params);
    }
    $stmt->execute();
    $row = $stmt->get_result()->fetch_row();
    $stmt->close();
    return $row[0] ?? null;
}

function rows(mysqli $db, string $sql, string $types = '', mixed ...$params): array
{
    $stmt = $db->prepare($sql);
    if ($types !== '') {
        $stmt->bind_param($types, ...$params);
    }
    $stmt->execute();
    $result = $stmt->get_result();
    $rows = [];
    while ($row = $result->fetch_assoc()) {
        $rows[] = $row;
    }
    $stmt->close();
    return $rows;
}

function table_exists(mysqli $db, string $schema, string $table): bool
{
    return (int)scalar(
        $db,
        'SELECT COUNT(*) FROM information_schema.tables WHERE table_schema=? AND table_name=?',
        'ss',
        $schema,
        $table
    ) > 0;
}

function table_count(mysqli $db, string $schema, string $table): int
{
    if (!table_exists($db, $schema, $table)) {
        return 0;
    }
    $safeSchema = str_replace('`', '``', $schema);
    $safeTable = str_replace('`', '``', $table);
    return (int)scalar($db, "SELECT COUNT(*) FROM `{$safeSchema}`.`{$safeTable}`");
}

function server_processes(string $root): array
{
    $names = ['login-server', 'char-server', 'map-server', 'web-server', 'voice-server'];
    $items = [];
    foreach ($names as $name) {
        $pidFile = $root . '/.' . $name . '.pid';
        $pid = is_file($pidFile) ? trim((string)file_get_contents($pidFile)) : '';
        $running = $pid !== '' && is_dir('/proc/' . $pid);
        $cmd = $running ? trim((string)@file_get_contents('/proc/' . $pid . '/comm')) : '';
        $cwd = $running ? (string)@readlink('/proc/' . $pid . '/cwd') : '';
        $items[] = [
            'name' => $name,
            'pid' => $pid,
            'running' => $running && $cmd === $name && $cwd === $root,
            'cwd' => $cwd,
        ];
    }
    return $items;
}

function command_lines(string $command): array
{
    $out = shell_exec($command . ' 2>/dev/null');
    if (!is_string($out) || trim($out) === '') {
        return [];
    }
    return array_values(array_filter(array_map('trim', explode("\n", $out))));
}

function api_payload(string $root, array $conf): array
{
    $ragDbName = $conf['char_server_db'] ?? $conf['map_server_db'] ?? '';
    $logDbName = $conf['log_db_db'] ?? '';
    $rag = db_connect($conf, 'char_server');
    $log = db_connect($conf, 'log_db');

    $payload = [
        'generated_at' => date('Y-m-d H:i:s'),
        'server' => [
            'processes' => server_processes($root),
            'safezone_processes' => command_lines("ps -u " . escapeshellarg((string)get_current_user()) . " -o pid,stat,lstart,cmd | grep -E './(login-server|char-server|map-server|web-server|voice-server)$' | grep -v grep"),
            'ports' => command_lines("ss -ltnp | grep -E 'login-server|char-server|map-server|web-server|voice-server|:22621|:22622|:22623|:31910|:7000'"),
        ],
        'core' => [
            'accounts' => table_count($rag, $ragDbName, 'login'),
            'characters' => table_count($rag, $ragDbName, 'char'),
            'online' => (int)scalar($rag, "SELECT COUNT(*) FROM `{$ragDbName}`.`char` WHERE online=1"),
            'guilds' => table_count($rag, $ragDbName, 'guild'),
            'inventory_rows' => table_count($rag, $ragDbName, 'inventory'),
            'storage_rows' => table_count($rag, $ragDbName, 'storage'),
        ],
        'economy' => rows($rag, "SELECT COALESCE(SUM(zeny),0) total_zeny, COALESCE(AVG(zeny),0) avg_zeny, COALESCE(MAX(zeny),0) max_zeny FROM `{$ragDbName}`.`char`"),
        'economy_detail' => [
            'top_zeny' => rows($rag, "SELECT char_id, name, zeny, last_map, online FROM `{$ragDbName}`.`char` ORDER BY zeny DESC, char_id ASC LIMIT 10"),
            'payment_by_status' => table_exists($rag, $ragDbName, 'payment')
                ? rows($rag, "SELECT status, COUNT(*) total, COALESCE(SUM(amount),0) amount, MAX(added_time) last_time FROM `{$ragDbName}`.`payment` GROUP BY status ORDER BY amount DESC")
                : [],
            'cashlog_by_type' => table_exists($log, $logDbName, 'cashlog')
                ? rows($log, "SELECT type, cash_type, COUNT(*) total, COALESCE(SUM(amount),0) amount, MAX(time) last_time FROM `{$logDbName}`.`cashlog` GROUP BY type,cash_type ORDER BY ABS(amount) DESC, total DESC LIMIT 12")
                : [],
            'picklog_24h_by_type' => table_exists($log, $logDbName, 'picklog')
                ? rows($log, "SELECT type, COUNT(*) total, COALESCE(SUM(amount),0) amount, MAX(time) last_time FROM `{$logDbName}`.`picklog` WHERE time >= DATE_SUB(NOW(), INTERVAL 24 HOUR) GROUP BY type ORDER BY total DESC")
                : [],
            'top_item_flow_24h' => table_exists($log, $logDbName, 'picklog')
                ? rows($log, "SELECT nameid, type, COALESCE(SUM(amount),0) amount, COUNT(*) total, MAX(time) last_time FROM `{$logDbName}`.`picklog` WHERE time >= DATE_SUB(NOW(), INTERVAL 24 HOUR) GROUP BY nameid,type ORDER BY ABS(amount) DESC, total DESC LIMIT 20")
                : [],
        ],
        'item_supply' => [
            'inventory_top' => table_exists($rag, $ragDbName, 'inventory')
                ? rows($rag, "SELECT nameid, COALESCE(SUM(amount),0) amount, COUNT(*) stacks FROM `{$ragDbName}`.`inventory` GROUP BY nameid ORDER BY amount DESC LIMIT 20")
                : [],
            'storage_top' => table_exists($rag, $ragDbName, 'storage')
                ? rows($rag, "SELECT nameid, COALESCE(SUM(amount),0) amount, COUNT(*) stacks FROM `{$ragDbName}`.`storage` GROUP BY nameid ORDER BY amount DESC LIMIT 20")
                : [],
            'cart_top' => table_exists($rag, $ragDbName, 'cart_inventory')
                ? rows($rag, "SELECT nameid, COALESCE(SUM(amount),0) amount, COUNT(*) stacks FROM `{$ragDbName}`.`cart_inventory` GROUP BY nameid ORDER BY amount DESC LIMIT 20")
                : [],
        ],
        'economy_history' => table_exists($rag, $ragDbName, 'admin_economy_snapshot')
            ? [
                'summary' => rows($rag, "SELECT id, created_at, total_zeny, max_zeny, online_chars, inventory_rows, picklog_rows, cashlog_rows, payment_rows, global_drop_rows FROM `{$ragDbName}`.`admin_economy_snapshot` ORDER BY id DESC LIMIT 48"),
                'latest_supply' => rows($rag, "SELECT source, nameid, amount, stacks FROM `{$ragDbName}`.`admin_item_supply_snapshot` WHERE snapshot_id = (SELECT MAX(id) FROM `{$ragDbName}`.`admin_economy_snapshot`) ORDER BY amount DESC LIMIT 30"),
                'latest_flow' => rows($rag, "SELECT nameid, type, amount, rows_count FROM `{$ragDbName}`.`admin_item_flow_snapshot` WHERE snapshot_id = (SELECT MAX(id) FROM `{$ragDbName}`.`admin_economy_snapshot`) ORDER BY ABS(amount) DESC, rows_count DESC LIMIT 30"),
            ]
            : ['summary' => [], 'latest_supply' => [], 'latest_flow' => []],
        'economy_alerts' => table_exists($rag, $ragDbName, 'admin_economy_alert')
            ? [
                'recent' => rows($rag, "SELECT created_at, severity, metric, message, value_now, value_prev, delta_value FROM `{$ragDbName}`.`admin_economy_alert` ORDER BY id DESC LIMIT 30"),
                'summary_24h' => rows($rag, "SELECT severity, COUNT(*) total FROM `{$ragDbName}`.`admin_economy_alert` WHERE created_at >= DATE_SUB(NOW(), INTERVAL 24 HOUR) GROUP BY severity ORDER BY FIELD(severity,'critical','warn','info')"),
            ]
            : ['recent' => [], 'summary_24h' => []],
        'online_by_map' => rows($rag, "SELECT COALESCE(last_map,'?') map, COUNT(*) total FROM `{$ragDbName}`.`char` WHERE online=1 GROUP BY last_map ORDER BY total DESC LIMIT 12"),
        'class_distribution' => rows($rag, "SELECT class, COUNT(*) total, MIN(base_level) min_lv, MAX(base_level) max_lv FROM `{$ragDbName}`.`char` GROUP BY class ORDER BY total DESC, class LIMIT 20"),
        'ai' => table_exists($rag, $ragDbName, 'aa_runtime_status')
            ? [
                'summary' => rows($rag, "SELECT ai_active, state, map, COUNT(*) total, COALESCE(SUM(zeny),0) zeny, COALESCE(SUM(cash),0) cash, MAX(updated_at) updated_at FROM `{$ragDbName}`.`aa_runtime_status` GROUP BY ai_active,state,map ORDER BY total DESC LIMIT 20"),
                'live' => rows($rag, "SELECT char_id, char_name, ai_active, state, map, hp, max_hp, sp, max_sp, weight, max_weight, target_name, updated_at FROM `{$ragDbName}`.`aa_runtime_status` ORDER BY updated_at DESC LIMIT 30"),
            ]
            : ['summary' => [], 'live' => []],
        'stamina' => table_exists($rag, $ragDbName, 'stamina')
            ? rows($rag, "SELECT COUNT(*) accounts, MIN(stamina_time) min_time, ROUND(AVG(stamina_time),2) avg_time, MAX(stamina_time) max_time, COALESCE(SUM(stamina_lock),0) locked FROM `{$ragDbName}`.`stamina`")
            : [],
        'logs' => [
            'picklog' => table_count($log, $logDbName, 'picklog'),
            'cashlog' => table_count($log, $logDbName, 'cashlog'),
            'zenylog' => table_count($log, $logDbName, 'zenylog'),
            'atcommandlog' => table_count($log, $logDbName, 'atcommandlog'),
            'loginlog' => table_count($log, $logDbName, 'loginlog'),
            'mvplog' => table_count($log, $logDbName, 'mvplog'),
        ],
        'resources' => [
            'global_drop' => table_count($rag, $ragDbName, 'global_drop'),
            'payment' => table_count($rag, $ragDbName, 'payment'),
            'vendings' => table_count($rag, $ragDbName, 'vendings'),
            'buyingstores' => table_count($rag, $ragDbName, 'buyingstores'),
            'server_mob_drops' => table_count($rag, $ragDbName, 'server_mob_drops'),
        ],
    ];

    $rag->close();
    $log->close();
    return $payload;
}

require_key($keyFile);
$conf = parse_conf($interConf);

if (isset($_GET['api'])) {
    header('Content-Type: application/json; charset=utf-8');
    try {
        echo json_encode(api_payload($root, $conf), JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE);
    } catch (Throwable $e) {
        http_response_code(500);
        echo json_encode(['error' => $e->getMessage()]);
    }
    exit;
}

$key = htmlspecialchars((string)$_GET['key'], ENT_QUOTES, 'UTF-8');
?>
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>SafeZone Admin Monitor</title>
<style>
:root{color-scheme:dark;--bg:#101214;--panel:#181b1f;--line:#2a3037;--text:#edf2f7;--muted:#9aa8b5;--good:#48d597;--bad:#ff6b6b;--warn:#f4c95d;--blue:#77b7ff}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font-family:Arial,Tahoma,sans-serif;font-size:14px}
header{display:flex;align-items:center;justify-content:space-between;padding:16px 20px;border-bottom:1px solid var(--line);background:#14171a;position:sticky;top:0;z-index:2}
h1{font-size:18px;margin:0}.muted{color:var(--muted)}main{padding:18px;display:grid;gap:14px}.grid{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:14px}.wide{grid-column:1/-1}
.panel{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:14px;min-width:0}.panel h2{font-size:14px;margin:0 0 12px;color:#d7e3ee}
.metric{font-size:28px;font-weight:700}.ok{color:var(--good)}.bad{color:var(--bad)}.warn{color:var(--warn)}.blue{color:var(--blue)}
table{width:100%;border-collapse:collapse}th,td{text-align:left;padding:7px 6px;border-bottom:1px solid var(--line);white-space:nowrap}th{color:var(--muted);font-weight:600}td:last-child,th:last-child{text-align:right}
.pill{display:inline-flex;align-items:center;border-radius:999px;padding:2px 8px;background:#222933;color:var(--muted);font-size:12px}.pill.ok{background:#153827;color:var(--good)}.pill.bad{background:#3a1c20;color:var(--bad)}
pre{margin:0;white-space:pre-wrap;color:#c7d2df;line-height:1.45}.error{background:#341b1f;border-color:#74313a;color:#ffd5db}
@media(max-width:1100px){.grid{grid-template-columns:repeat(2,minmax(0,1fr))}}@media(max-width:700px){header{align-items:flex-start;gap:8px;flex-direction:column}.grid{grid-template-columns:1fr}main{padding:12px}}
</style>
</head>
<body>
<header>
  <div><h1>SafeZone Admin Monitor</h1><div class="muted">Read-only live view</div></div>
  <div class="muted">Last update: <span id="updated">loading</span></div>
</header>
<main id="app">
  <div class="panel">Loading monitor data...</div>
</main>
<script>
const key = <?= json_encode($key) ?>;
const app = document.getElementById("app");
const updated = document.getElementById("updated");
function esc(v){return String(v ?? "").replace(/[&<>"']/g,m=>({"&":"&amp;","<":"&lt;",">":"&gt;","\"":"&quot;","'":"&#039;"}[m]));}
function num(v){return Number(v || 0).toLocaleString("en-US");}
function table(rows, cols){
  if(!rows || !rows.length) return '<div class="muted">No data</div>';
  return '<table><thead><tr>'+cols.map(c=>`<th>${esc(c[0])}</th>`).join('')+'</tr></thead><tbody>'+
    rows.map(r=>'<tr>'+cols.map(c=>`<td>${esc(typeof c[1]==='function'?c[1](r):r[c[1]])}</td>`).join('')+'</tr>').join('')+
    '</tbody></table>';
}
async function load(){
  try{
    const res = await fetch(`?api=1&key=${encodeURIComponent(key)}`, {cache:"no-store"});
    const data = await res.json();
    if(data.error) throw new Error(data.error);
    updated.textContent = data.generated_at;
    const econ = data.economy?.[0] || {};
    const stamina = data.stamina?.[0] || {};
    const down = data.server.processes.filter(p=>!p.running);
    const alertSummary = Object.fromEntries((data.economy_alerts?.summary_24h || []).map(r=>[r.severity, Number(r.total||0)]));
    app.innerHTML = `
      <section class="grid">
        <div class="panel"><h2>Online</h2><div class="metric ${data.core.online>0?'ok':'warn'}">${num(data.core.online)}</div><div class="muted">${num(data.core.characters)} chars / ${num(data.core.accounts)} accounts</div></div>
        <div class="panel"><h2>Total Zeny</h2><div class="metric blue">${num(econ.total_zeny)}</div><div class="muted">max ${num(econ.max_zeny)} / avg ${num(Math.round(econ.avg_zeny||0))}</div></div>
        <div class="panel"><h2>AI Active</h2><div class="metric ok">${num((data.ai.summary||[]).reduce((a,r)=>a+Number(r.total||0),0))}</div><div class="muted">runtime rows</div></div>
        <div class="panel"><h2>Alerts 24h</h2><div class="metric ${(alertSummary.critical||0)>0?'bad':((alertSummary.warn||0)>0?'warn':'ok')}">${num((alertSummary.critical||0)+(alertSummary.warn||0))}</div><div class="muted">critical ${num(alertSummary.critical||0)} / warn ${num(alertSummary.warn||0)}</div></div>
      </section>
      <section class="grid">
        <div class="panel"><h2>Processes</h2>${table(data.server.processes,[['Name','name'],['PID','pid'],['State',r=>r.running?'OK':'DOWN']])}</div>
        <div class="panel"><h2>Logs</h2>${table(Object.entries(data.logs).map(([k,v])=>({k,v})),[['Table','k'],['Rows',r=>num(r.v)]])}</div>
        <div class="panel"><h2>Resources</h2>${table(Object.entries(data.resources).map(([k,v])=>({k,v})),[['Table','k'],['Rows',r=>num(r.v)]])}</div>
        <div class="panel"><h2>Stamina</h2>${table([stamina],[['Accounts',r=>num(r.accounts)],['Avg',r=>num(r.avg_time)],['Locked',r=>num(r.locked)]])}</div>
      </section>
      <section class="grid">
        <div class="panel"><h2>Online By Map</h2>${table(data.online_by_map,[['Map','map'],['Online',r=>num(r.total)]])}</div>
        <div class="panel"><h2>Class Distribution</h2>${table(data.class_distribution,[['Class','class'],['Total',r=>num(r.total)],['Lv',r=>`${r.min_lv}-${r.max_lv}`]])}</div>
        <div class="panel wide"><h2>AI Runtime</h2>${table(data.ai.live,[['Char','char_name'],['State','state'],['Map','map'],['HP',r=>`${num(r.hp)}/${num(r.max_hp)}`],['SP',r=>`${num(r.sp)}/${num(r.max_sp)}`],['Weight',r=>`${num(r.weight)}/${num(r.max_weight)}`],['Target','target_name'],['Updated','updated_at']])}</div>
      </section>
      <section class="grid">
        <div class="panel"><h2>Top Zeny</h2>${table(data.economy_detail.top_zeny,[['Char','name'],['Zeny',r=>num(r.zeny)],['Map','last_map'],['On',r=>r.online?'Y':'N']])}</div>
        <div class="panel"><h2>Payment</h2>${table(data.economy_detail.payment_by_status,[['Status','status'],['Count',r=>num(r.total)],['Amount',r=>num(r.amount)],['Last','last_time']])}</div>
        <div class="panel"><h2>Cash Movement</h2>${table(data.economy_detail.cashlog_by_type,[['Type',r=>`${r.type}/${r.cash_type}`],['Count',r=>num(r.total)],['Amount',r=>num(r.amount)],['Last','last_time']])}</div>
        <div class="panel"><h2>Item Flow 24h</h2>${table(data.economy_detail.top_item_flow_24h,[['Item','nameid'],['Type','type'],['Amount',r=>num(r.amount)],['Rows',r=>num(r.total)]])}</div>
      </section>
      <section class="grid">
        <div class="panel"><h2>Inventory Supply</h2>${table(data.item_supply.inventory_top,[['Item','nameid'],['Amount',r=>num(r.amount)],['Stacks',r=>num(r.stacks)]])}</div>
        <div class="panel"><h2>Storage Supply</h2>${table(data.item_supply.storage_top,[['Item','nameid'],['Amount',r=>num(r.amount)],['Stacks',r=>num(r.stacks)]])}</div>
        <div class="panel"><h2>Cart Supply</h2>${table(data.item_supply.cart_top,[['Item','nameid'],['Amount',r=>num(r.amount)],['Stacks',r=>num(r.stacks)]])}</div>
        <div class="panel"><h2>Picklog 24h</h2>${table(data.economy_detail.picklog_24h_by_type,[['Type','type'],['Rows',r=>num(r.total)],['Amount',r=>num(r.amount)],['Last','last_time']])}</div>
      </section>
      <section class="grid">
        <div class="panel wide"><h2>Economy Alerts</h2>${table(data.economy_alerts.recent,[['Time','created_at'],['Severity','severity'],['Metric','metric'],['Message','message'],['Now',r=>num(r.value_now)],['Prev',r=>num(r.value_prev)],['Delta',r=>num(r.delta_value)]])}</div>
        <div class="panel wide"><h2>Economy Snapshots</h2>${table(data.economy_history.summary,[['Time','created_at'],['Zeny',r=>num(r.total_zeny)],['Max',r=>num(r.max_zeny)],['Online',r=>num(r.online_chars)],['Pick Rows',r=>num(r.picklog_rows)],['Cash Rows',r=>num(r.cashlog_rows)],['Payments',r=>num(r.payment_rows)]])}</div>
        <div class="panel"><h2>Snapshot Supply</h2>${table(data.economy_history.latest_supply,[['Source','source'],['Item','nameid'],['Amount',r=>num(r.amount)],['Stacks',r=>num(r.stacks)]])}</div>
        <div class="panel"><h2>Snapshot Flow 10m</h2>${table(data.economy_history.latest_flow,[['Item','nameid'],['Type','type'],['Amount',r=>num(r.amount)],['Rows',r=>num(r.rows_count)]])}</div>
      </section>
      <section class="grid">
        <div class="panel wide"><h2>Listening Ports</h2><pre>${esc((data.server.ports||[]).join("\\n") || "No port data")}</pre></div>
      </section>`;
  }catch(e){
    app.innerHTML = `<div class="panel error">Monitor error: ${esc(e.message)}</div>`;
  }
}
load();
setInterval(load, 5000);
</script>
</body>
</html>
