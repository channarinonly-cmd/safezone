<?php
require '../template/header.php';
require '../library/database.php';

if (!isset($_SESSION["session_user"]) || $_SESSION["session_user"] !== TRUE) {
    header("location: " . $app_url . "/login.php");
    exit;
}

$account_id = (int)$_SESSION['account_id'];
$message = '';
$error = '';

function aa_ints($value): array
{
    $out = [];
    foreach ((array)$value as $id) {
        $id = (int)$id;
        if ($id > 0) {
            $out[$id] = $id;
        }
    }
    return array_values($out);
}

function aa_simple_yml_map(string $kind): array
{
    static $cache = [];
    if (isset($cache[$kind])) {
        return $cache[$kind];
    }

    $patterns = $kind === 'skill'
        ? [__DIR__ . '/../../skill_db*.yml', __DIR__ . '/../../db/*/skill_db*.yml', __DIR__ . '/../../db/skill_db*.yml']
        : [__DIR__ . '/../../item_db*.yml', __DIR__ . '/../../db/*/item_db*.yml', __DIR__ . '/../../db/item_db*.yml'];

    $names = [];
    foreach ($patterns as $pattern) {
        foreach (glob($pattern) ?: [] as $file) {
            $currentId = 0;
            foreach (@file($file, FILE_IGNORE_NEW_LINES) ?: [] as $line) {
                if (preg_match('/^\s*-\s*Id:\s*(\d+)/', $line, $m)) {
                    $currentId = (int)$m[1];
                } elseif ($currentId > 0 && $kind === 'skill' && preg_match('/^\s*Description:\s*(.+?)\s*$/', $line, $m)) {
                    $names[$currentId] = trim($m[1], " \"'");
                    $currentId = 0;
                } elseif ($currentId > 0 && $kind === 'item' && preg_match('/^\s*Name:\s*(.+?)\s*$/', $line, $m)) {
                    $names[$currentId] = trim($m[1], " \"'");
                    $currentId = 0;
                }
            }
        }
    }

    $cache[$kind] = $names;
    return $names;
}

function aa_item_name(mysqli $con, int $id): string
{
    static $cache = [];
    if (isset($cache[$id])) {
        return $cache[$id];
    }

    $names = aa_simple_yml_map('item');
    if (!empty($names[$id])) {
        return $cache[$id] = $names[$id];
    }

    $name = '';
    foreach (['item_db', 'item_db_re', 'item_db2', 'item_db2_re'] as $table) {
        $res = @$con->query("SELECT `name_japanese`,`name_english` FROM `{$table}` WHERE `id`={$id} LIMIT 1");
        if ($res && $res->num_rows > 0) {
            $row = $res->fetch_assoc();
            $name = $row['name_english'] ?: $row['name_japanese'];
            break;
        }
    }

    if ($name && strpos($name, '_') !== false) {
        $name = str_replace('_', ' ', $name);
    }

    return $cache[$id] = ($name ?: 'Item #' . $id);
}

function aa_skill_name(mysqli $con, int $id): string
{
    static $cache = [];
    if (isset($cache[$id])) {
        return $cache[$id];
    }

    $names = aa_simple_yml_map('skill');
    if (!empty($names[$id])) {
        return $cache[$id] = $names[$id];
    }

    $name = '';
    foreach (['skill_db', 'skill_db_re'] as $table) {
        $res = @$con->query("SELECT `desc`,`name` FROM `{$table}` WHERE `id`={$id} LIMIT 1");
        if ($res && $res->num_rows > 0) {
            $row = $res->fetch_assoc();
            $name = $row['desc'] ?: $row['name'];
            break;
        }
    }

    if ($name && strpos($name, '_') !== false) {
        $name = str_replace('_', ' ', $name);
    }

    return $cache[$id] = ($name ?: 'Skill #' . $id);
}

function aa_buff_item_ids(): array
{
    static $ids = null;
    if ($ids !== null) {
        return $ids;
    }

    $ids = [];
    foreach ([__DIR__ . '/../../ai_item_buff.txt', __DIR__ . '/../../db/custom/ai_item_buff.txt'] as $file) {
        foreach (@file($file, FILE_IGNORE_NEW_LINES) ?: [] as $line) {
            $line = trim($line);
            if ($line === '' || strpos($line, '//') === 0) {
                continue;
            }
            $parts = explode(',', $line);
            $itemId = (int)($parts[0] ?? 0);
            if ($itemId > 0) {
                $ids[$itemId] = true;
            }
        }
    }

    return $ids;
}

function aa_usable_item_ids(): array
{
    static $ids = null;
    if ($ids !== null) {
        return $ids;
    }

    $ids = [];
    foreach ([__DIR__ . '/../../item_db_usable.yml', __DIR__ . '/../../db/re/item_db_usable.yml'] as $file) {
        foreach (@file($file, FILE_IGNORE_NEW_LINES) ?: [] as $line) {
            if (preg_match('/^\s*-\s*Id:\s*(\d+)/', $line, $m)) {
                $ids[(int)$m[1]] = true;
            }
        }
    }

    return $ids;
}

function aa_pick_buy_rows(array $enabled, array $mins, array $targets): array
{
    $rows = [];
    foreach (aa_ints($enabled) as $id) {
        $min = max(0, min(99999, (int)($mins[$id] ?? 0)));
        $target = max(0, min(99999, (int)($targets[$id] ?? 0)));
        if ($target > $min) {
            $rows[$id] = ['item_id' => $id, 'min_amount' => $min, 'target_amount' => $target];
        }
    }
    return array_values($rows);
}

$chars = $con->query("SELECT char_id, `name` FROM `char` WHERE account_id={$account_id} ORDER BY char_num, char_id");
$char_list = [];
while ($chars && ($char = $chars->fetch_object())) {
    $char_list[] = $char;
}

$selected_char_id = isset($_POST['char_id']) ? (int)$_POST['char_id'] : (isset($_GET['char_id']) ? (int)$_GET['char_id'] : 0);
if (!$selected_char_id && !empty($char_list)) {
    $selected_char_id = (int)$char_list[0]->char_id;
}

$owns_char = false;
foreach ($char_list as $char) {
    if ((int)$char->char_id === $selected_char_id) {
        $owns_char = true;
        break;
    }
}

if (empty($char_list)) {
    $error = 'ยังไม่มีตัวละครในบัญชีนี้';
} elseif (!$owns_char) {
    $error = 'ตัวละครนี้ไม่ตรงกับบัญชีของคุณ';
}

if (!$error && $_SERVER['REQUEST_METHOD'] === 'POST') {
    if (isset($_POST['reset_ai'])) {
        $con->query("DELETE FROM `aa_items` WHERE `char_id`={$selected_char_id}");
        $con->query("DELETE FROM `aa_skills` WHERE `char_id`={$selected_char_id}");
        $con->query("DELETE FROM `aa_mobs` WHERE `char_id`={$selected_char_id}");
        $con->query("DELETE FROM `aa_flee_mobs` WHERE `char_id`={$selected_char_id}");
        $con->query("DELETE FROM `aa_common_config` WHERE `char_id`={$selected_char_id}");
        $message = 'ล้างค่าทั้งหมดแล้ว';
    } elseif (isset($_POST['save_ai'])) {
        $pickup_mode = max(0, min(2, (int)($_POST['pickup_mode'] ?? 0)));
        $skill_rate = max(0, min(100, (int)($_POST['skill_rate'] ?? 20)));
        $swarm_count = max(0, min(50, (int)($_POST['swarm_wing_count'] ?? 4)));
        $swarm_enabled = isset($_POST['swarm_wing_enabled']) ? 1 : 0;

        $con->query("START TRANSACTION");
        $ok = true;

        $ok = $ok && $con->query(
            "INSERT INTO `aa_common_config` (`char_id`,`stopmelee`,`pickup_item_config`,`aggressive_behavior`,`autositregen_conf`,`autositregen_maxhp`,`autositregen_minhp`,`autositregen_maxsp`,`autositregen_minsp`,`tp_use_teleport`,`tp_use_flywing`,`tp_min_hp`,`tp_delay_nomobmeet`,`skill_rate`,`teleport_boss`,`tp_swarm_enable`,`tp_swarm_count`,`focus_mob`,`stay_mode`,`revive_auto`,`party_auto`,`heal_party`,`heal_hp`) " .
            "VALUES ({$selected_char_id},0,{$pickup_mode},0,0,0,0,0,0,0,1,0,0,{$skill_rate},1,{$swarm_enabled},{$swarm_count},0,0,0,0,0,0) " .
            "ON DUPLICATE KEY UPDATE `pickup_item_config`=VALUES(`pickup_item_config`),`skill_rate`=VALUES(`skill_rate`),`tp_swarm_enable`=VALUES(`tp_swarm_enable`),`tp_swarm_count`=VALUES(`tp_swarm_count`),`focus_mob`=0"
        );

        $ok = $ok && $con->query("DELETE FROM `aa_items` WHERE `char_id`={$selected_char_id} AND `type` IN (0,2,3,4)");
        $ok = $ok && $con->query("DELETE FROM `aa_skills` WHERE `char_id`={$selected_char_id} AND `type`=2");

        $buffAllowed = aa_buff_item_ids();
        foreach (aa_ints($_POST['buff_items'] ?? []) as $id) {
            if (!isset($buffAllowed[$id])) {
                continue;
            }
            $ok = $ok && $con->query("INSERT INTO `aa_items` (`char_id`,`type`,`item_id`) VALUES ({$selected_char_id},0,{$id})");
        }
        foreach (aa_ints($_POST['pickup_items'] ?? []) as $id) {
            $ok = $ok && $con->query("INSERT INTO `aa_items` (`char_id`,`type`,`item_id`) VALUES ({$selected_char_id},2,{$id})");
        }
        foreach (aa_ints($_POST['keep_items'] ?? []) as $id) {
            $ok = $ok && $con->query("INSERT INTO `aa_items` (`char_id`,`type`,`item_id`) VALUES ({$selected_char_id},3,{$id})");
        }
        foreach (aa_pick_buy_rows($_POST['buy_items'] ?? [], $_POST['buy_min'] ?? [], $_POST['buy_target'] ?? []) as $row) {
            $ok = $ok && $con->query("INSERT INTO `aa_items` (`char_id`,`type`,`item_id`,`min_hp`,`min_sp`) VALUES ({$selected_char_id},4,{$row['item_id']},{$row['min_amount']},{$row['target_amount']})");
        }

        foreach (aa_ints($_POST['attack_skills'] ?? []) as $skill_id) {
            $lv = max(1, min(20, (int)($_POST['skill_lv'][$skill_id] ?? 1)));
            $swarm = max(0, min(50, (int)($_POST['skill_swarm'][$skill_id] ?? 0)));
            $ok = $ok && $con->query("INSERT INTO `aa_skills` (`char_id`,`type`,`skill_id`,`skill_lv`,`min_hp`) VALUES ({$selected_char_id},2,{$skill_id},{$lv},{$swarm})");
        }

        if ($ok) {
            $con->query("COMMIT");
            $message = 'บันทึกแล้ว ถ้าตัวละครออนไลน์อยู่ให้ปิดเปิด AI ใหม่เพื่อโหลดค่า';
        } else {
            $con->query("ROLLBACK");
            $error = 'บันทึกไม่สำเร็จ: ' . htmlspecialchars($con->error);
        }
    }
}

$common = (object)[
    'pickup_item_config' => 0,
    'skill_rate' => 20,
    'tp_swarm_enable' => 1,
    'tp_swarm_count' => 4,
];
$res = $con->query("SELECT `pickup_item_config`,`skill_rate`,`tp_swarm_enable`,`tp_swarm_count` FROM `aa_common_config` WHERE `char_id`={$selected_char_id} LIMIT 1");
if ($res && $res->num_rows > 0) {
    $common = $res->fetch_object();
}

$selected = ['buff' => [], 'pickup' => [], 'keep' => [], 'buy' => [], 'skills' => []];
$res = $con->query("SELECT `type`,`item_id`,`min_hp`,`min_sp` FROM `aa_items` WHERE `char_id`={$selected_char_id}");
while ($res && ($row = $res->fetch_object())) {
    if ((int)$row->type === 0) $selected['buff'][(int)$row->item_id] = true;
    if ((int)$row->type === 2) $selected['pickup'][(int)$row->item_id] = true;
    if ((int)$row->type === 3) $selected['keep'][(int)$row->item_id] = true;
    if ((int)$row->type === 4) $selected['buy'][(int)$row->item_id] = ['min' => (int)$row->min_hp, 'target' => (int)$row->min_sp];
}
$res = $con->query("SELECT `skill_id`,`skill_lv`,`min_hp` FROM `aa_skills` WHERE `char_id`={$selected_char_id} AND `type`=2");
while ($res && ($row = $res->fetch_object())) {
    $selected['skills'][(int)$row->skill_id] = ['lv' => (int)$row->skill_lv, 'swarm' => (int)$row->min_hp];
}

$inventory = [];
$buffIds = aa_buff_item_ids();
$usableIds = aa_usable_item_ids();
$res = $con->query("SELECT `nameid`,SUM(`amount`) AS amount FROM `inventory` WHERE `char_id`={$selected_char_id} AND `nameid` > 0 GROUP BY `nameid` ORDER BY `nameid` LIMIT 300");
while ($res && ($row = $res->fetch_object())) {
    $id = (int)$row->nameid;
    $inventory[$id] = ['id' => $id, 'amount' => (int)$row->amount, 'name' => aa_item_name($con, $id)];
}

$buffItems = [];
$usableItems = [];
foreach ($inventory as $item) {
    if (isset($buffIds[$item['id']])) {
        $buffItems[] = $item;
    }
    if (isset($usableIds[$item['id']])) {
        $usableItems[] = $item;
    }
}
$buyItems = $usableItems;
foreach ($selected['buy'] as $id => $row) {
    if (!isset($inventory[$id])) {
        $buyItems[] = ['id' => $id, 'amount' => 0, 'name' => aa_item_name($con, $id)];
    }
}

$skills = [];
$res = $con->query("SELECT `id`,`lv` FROM `skill` WHERE `char_id`={$selected_char_id} AND `id` > 0 AND `lv` > 0 ORDER BY `id` LIMIT 160");
while ($res && ($row = $res->fetch_object())) {
    $skills[] = ['id' => (int)$row->id, 'lv' => (int)$row->lv, 'name' => aa_skill_name($con, (int)$row->id)];
}
?>
<style>
.ai-panel{background:#fff;border:1px solid #e3e7ee;border-radius:8px;padding:16px;margin-bottom:14px}
.ai-title{font-weight:700;margin:0 0 10px;color:#1f2937}
.ai-muted{color:#6b7280;font-size:13px}
.ai-item-row{display:grid;grid-template-columns:1fr;gap:8px;border-top:1px solid #eef1f5;padding:10px 0}
.ai-item-row:first-child{border-top:0}
.ai-name{font-weight:700;color:#111827}
.ai-actions{display:flex;flex-wrap:wrap;gap:12px;align-items:center}
.ai-actions label{margin:0}
.ai-small-input{width:92px;display:inline-block}
.ai-toolbar{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin-bottom:12px}
.ai-search{max-width:360px}
@media (min-width: 768px){.ai-item-row{grid-template-columns:minmax(220px,1fr) 2fr}.ai-actions{justify-content:flex-end}}
</style>
<section class="fxt-template-animation fxt-template-layout22" data-bg-image="<?= $app_url ?>/assets/img/figure/bg.jpg">
    <div class="star-animation"><div id="stars4"></div><div id="stars5"></div></div>
    <div class="container">
        <div class="row align-items-center justify-content-md-center shadow-lg">
            <div class="col-lg-12 fxt-bg-color">
                <div class="fxt-content">
                    <div class="fxt-form">
                        <h2>ตั้งค่า AI ฟาร์ม</h2>
                        <p class="ai-muted">เลือกจากรายการได้เลย ไม่ต้องจำเลขไอเทมหรือเลขสกิล</p>
                        <?php require_once '../template/menu.php' ?>

                        <div class="shadow p-3 mb-3 bg-body rounded">
                            <?php if ($message) : ?><div class="alert alert-success"><?= $message ?></div><?php endif; ?>
                            <?php if ($error) : ?><div class="alert alert-warning"><?= $error ?></div><?php endif; ?>

                            <?php if (!empty($char_list)) : ?>
                                <form method="get" class="ai-panel">
                                    <label class="form-label fw-bold">ตัวละคร</label>
                                    <div class="input-group">
                                        <select name="char_id" class="form-select">
                                            <?php foreach ($char_list as $char) : ?>
                                                <option value="<?= (int)$char->char_id ?>" <?= (int)$char->char_id === $selected_char_id ? 'selected' : '' ?>><?= htmlspecialchars($char->name) ?></option>
                                            <?php endforeach; ?>
                                        </select>
                                        <button class="btn btn-dark" type="submit">โหลดค่า</button>
                                    </div>
                                </form>
                            <?php endif; ?>

                            <?php if ($owns_char) : ?>
                                <form method="post" id="aiForm">
                                    <input type="hidden" name="char_id" value="<?= (int)$selected_char_id ?>">

                                    <div class="ai-panel">
                                        <h5 class="ai-title">ตั้งค่าหลัก</h5>
                                        <div class="row">
                                            <div class="col-lg-4 mb-3">
                                                <label class="form-label fw-bold">โหมดเก็บของ</label>
                                                <select name="pickup_mode" class="form-select">
                                                    <option value="0" <?= (int)$common->pickup_item_config === 0 ? 'selected' : '' ?>>เก็บทุกอย่าง</option>
                                                    <option value="1" <?= (int)$common->pickup_item_config === 1 ? 'selected' : '' ?>>เก็บเฉพาะที่เลือก</option>
                                                    <option value="2" <?= (int)$common->pickup_item_config === 2 ? 'selected' : '' ?>>ไม่เก็บของ</option>
                                                </select>
                                            </div>
                                            <div class="col-lg-4 mb-3">
                                                <label class="form-label fw-bold">โอกาสใช้สกิลโจมตี (%)</label>
                                                <input name="skill_rate" class="form-control" type="number" min="0" max="100" value="<?= (int)$common->skill_rate ?>">
                                            </div>
                                            <div class="col-lg-4 mb-3">
                                                <label class="form-label fw-bold">โดนมอนรุมกี่ตัวให้วิง</label>
                                                <div class="input-group">
                                                    <span class="input-group-text"><input type="checkbox" name="swarm_wing_enabled" <?= (int)$common->tp_swarm_enable ? 'checked' : '' ?>></span>
                                                    <input name="swarm_wing_count" class="form-control" type="number" min="0" max="50" value="<?= (int)$common->tp_swarm_count ?>">
                                                </div>
                                            </div>
                                        </div>
                                    </div>

                                    <div class="ai-panel">
                                        <div class="ai-toolbar">
                                            <h5 class="ai-title mb-0">ยาบัฟ</h5>
                                            <span class="ai-muted">แสดงเฉพาะของในตัวที่อยู่ใน ai_item_buff.txt</span>
                                        </div>
                                        <?php if (empty($buffItems)) : ?>
                                            <div class="alert alert-light border">ไม่พบยาบัฟในตัวละครนี้</div>
                                        <?php endif; ?>
                                        <?php foreach ($buffItems as $item) : ?>
                                            <div class="ai-item-row ai-filter-row" data-name="<?= htmlspecialchars(strtolower($item['name'] . ' ' . $item['id'])) ?>">
                                                <div><div class="ai-name"><?= htmlspecialchars($item['name']) ?></div><div class="ai-muted">ID <?= $item['id'] ?> / มี <?= $item['amount'] ?> ชิ้น</div></div>
                                                <div class="ai-actions">
                                                    <label><input type="checkbox" name="buff_items[]" value="<?= $item['id'] ?>" <?= isset($selected['buff'][$item['id']]) ? 'checked' : '' ?>> ใช้เป็นยาบัฟ</label>
                                                </div>
                                            </div>
                                        <?php endforeach; ?>
                                    </div>

                                    <div class="ai-panel">
                                        <div class="ai-toolbar">
                                            <h5 class="ai-title mb-0">สกิลโจมตี</h5>
                                            <input class="form-control ai-search" data-ai-search="skill" placeholder="ค้นหาสกิล">
                                        </div>
                                        <?php foreach ($skills as $skill) : $saved = $selected['skills'][$skill['id']] ?? null; ?>
                                            <div class="ai-item-row ai-filter-row" data-group="skill" data-name="<?= htmlspecialchars(strtolower($skill['name'] . ' ' . $skill['id'])) ?>">
                                                <div><div class="ai-name"><?= htmlspecialchars($skill['name']) ?></div><div class="ai-muted">ID <?= $skill['id'] ?> / Lv.<?= $skill['lv'] ?></div></div>
                                                <div class="ai-actions">
                                                    <label><input type="checkbox" name="attack_skills[]" value="<?= $skill['id'] ?>" <?= $saved ? 'checked' : '' ?>> ใช้สกิลนี้</label>
                                                    <label>เลเวล <input name="skill_lv[<?= $skill['id'] ?>]" class="form-control ai-small-input" type="number" min="1" max="<?= $skill['lv'] ?>" value="<?= $saved['lv'] ?? $skill['lv'] ?>"></label>
                                                    <label>โดนรุม <input name="skill_swarm[<?= $skill['id'] ?>]" class="form-control ai-small-input" type="number" min="0" max="50" value="<?= $saved['swarm'] ?? 0 ?>"></label>
                                                </div>
                                            </div>
                                        <?php endforeach; ?>
                                    </div>

                                    <div class="ai-panel">
                                        <div class="ai-toolbar">
                                            <h5 class="ai-title mb-0">เก็บของ / ไม่ฝากคลัง</h5>
                                            <input class="form-control ai-search" data-ai-search="item" placeholder="ค้นหาไอเทม">
                                        </div>
                                        <?php foreach ($inventory as $item) : ?>
                                            <div class="ai-item-row ai-filter-row" data-group="item" data-name="<?= htmlspecialchars(strtolower($item['name'] . ' ' . $item['id'])) ?>">
                                                <div><div class="ai-name"><?= htmlspecialchars($item['name']) ?></div><div class="ai-muted">ID <?= $item['id'] ?> / มี <?= $item['amount'] ?> ชิ้น</div></div>
                                                <div class="ai-actions">
                                                    <label><input type="checkbox" name="pickup_items[]" value="<?= $item['id'] ?>" <?= isset($selected['pickup'][$item['id']]) ? 'checked' : '' ?>> เก็บของนี้</label>
                                                    <label><input type="checkbox" name="keep_items[]" value="<?= $item['id'] ?>" <?= isset($selected['keep'][$item['id']]) ? 'checked' : '' ?>> ไม่ฝากคลัง</label>
                                                </div>
                                            </div>
                                        <?php endforeach; ?>
                                    </div>

                                    <div class="ai-panel">
                                        <div class="ai-toolbar">
                                            <h5 class="ai-title mb-0">ซื้อของอัตโนมัติ</h5>
                                            <span class="ai-muted">บอทจะกลับมาซื้อเมื่อของต่ำกว่าจำนวนขั้นต่ำ</span>
                                        </div>
                                        <?php if (empty($buyItems)) : ?>
                                            <div class="alert alert-light border">ไม่พบไอเทมใช้งานในตัวละครนี้</div>
                                        <?php endif; ?>
                                        <?php foreach ($buyItems as $item) : $buy = $selected['buy'][$item['id']] ?? ['min' => 0, 'target' => 0]; ?>
                                            <div class="ai-item-row ai-filter-row" data-name="<?= htmlspecialchars(strtolower($item['name'] . ' ' . $item['id'])) ?>">
                                                <div><div class="ai-name"><?= htmlspecialchars($item['name']) ?></div><div class="ai-muted">ID <?= $item['id'] ?> / มี <?= $item['amount'] ?> ชิ้น</div></div>
                                                <div class="ai-actions">
                                                    <label><input type="checkbox" name="buy_items[]" value="<?= $item['id'] ?>" <?= isset($selected['buy'][$item['id']]) ? 'checked' : '' ?>> ซื้ออัตโนมัติ</label>
                                                    <label>เหลือน้อยกว่า <input name="buy_min[<?= $item['id'] ?>]" class="form-control ai-small-input" type="number" min="0" max="99999" value="<?= (int)$buy['min'] ?>"></label>
                                                    <label>ซื้อให้ถึง <input name="buy_target[<?= $item['id'] ?>]" class="form-control ai-small-input" type="number" min="0" max="99999" value="<?= (int)$buy['target'] ?>"></label>
                                                </div>
                                            </div>
                                        <?php endforeach; ?>
                                    </div>

                                    <button name="save_ai" value="1" type="submit" class="btn btn-primary"><i class="fas fa-save"></i> บันทึกตั้งค่า AI</button>
                                    <button name="reset_ai" value="1" type="submit" class="btn btn-danger" onclick="return confirm('ล้างค่าทั้งหมด?')"><i class="fas fa-trash"></i> ล้างค่าทั้งหมด</button>
                                </form>
                            <?php endif; ?>
                        </div>
                    </div>
                    <div class="fxt-footer">
                        <p>ต้องการออกจากระบบใช่ไหม ? <a href="<?= $app_url ?>/logout.php" class="switcher-text2 inline-text">คลิกที่นี่</a></p>
                    </div>
                </div>
            </div>
        </div>
    </div>
</section>
<script>
document.querySelectorAll('[data-ai-search]').forEach(function (input) {
    input.addEventListener('input', function () {
        var group = input.getAttribute('data-ai-search');
        var q = input.value.trim().toLowerCase();
        document.querySelectorAll('.ai-filter-row[data-group="' + group + '"]').forEach(function (row) {
            row.style.display = row.getAttribute('data-name').indexOf(q) >= 0 ? '' : 'none';
        });
    });
});
</script>
<?php require '../template/footer.php'; ?>
