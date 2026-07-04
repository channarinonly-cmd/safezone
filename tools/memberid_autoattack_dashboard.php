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

function aa_name(mysqli $con, int $id): string
{
    static $cache = [];
    static $ymlNames = null;
    if (isset($cache[$id])) {
        return $cache[$id];
    }

    if ($ymlNames === null) {
        $ymlNames = [];
        foreach (glob(__DIR__ . '/../../item_db*.yml') ?: [] as $file) {
            $currentId = 0;
            foreach (@file($file, FILE_IGNORE_NEW_LINES) ?: [] as $line) {
                if (preg_match('/^\s*-\s*Id:\s*(\d+)/', $line, $m)) {
                    $currentId = (int)$m[1];
                } elseif ($currentId > 0 && preg_match('/^\s*Name:\s*(.+?)\s*$/', $line, $m)) {
                    $ymlNames[$currentId] = trim($m[1], " \"'");
                    $currentId = 0;
                }
            }
        }
    }

    if (!empty($ymlNames[$id])) {
        $cache[$id] = $ymlNames[$id];
        return $cache[$id];
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

    $cache[$id] = $name ?: ('Item #' . $id);
    return $cache[$id];
}

function aa_skill_name(mysqli $con, int $id): string
{
    static $cache = [];
    static $ymlNames = null;
    if (isset($cache[$id])) {
        return $cache[$id];
    }

    if ($ymlNames === null) {
        $ymlNames = [];
        $files = array_merge(
            glob(__DIR__ . '/../../skill_db*.yml') ?: [],
            glob(__DIR__ . '/../../db/*/skill_db*.yml') ?: [],
            glob(__DIR__ . '/../../db/skill_db*.yml') ?: []
        );
        foreach ($files as $file) {
            $currentId = 0;
            foreach (@file($file, FILE_IGNORE_NEW_LINES) ?: [] as $line) {
                if (preg_match('/^\s*-\s*Id:\s*(\d+)/', $line, $m)) {
                    $currentId = (int)$m[1];
                } elseif ($currentId > 0 && preg_match('/^\s*Description:\s*(.+?)\s*$/', $line, $m)) {
                    $ymlNames[$currentId] = trim($m[1], " \"'");
                    $currentId = 0;
                }
            }
        }
    }

    if (!empty($ymlNames[$id])) {
        $cache[$id] = $ymlNames[$id];
        return $cache[$id];
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

    $cache[$id] = $name ?: ('Skill #' . $id);
    return $cache[$id];
}

function aa_buy_rows($text): array
{
    $rows = [];
    foreach (preg_split('/\r\n|\r|\n/', (string)$text) as $line) {
        preg_match_all('/\d+/', $line, $matches);
        $parts = array_map('intval', $matches[0]);
        if (count($parts) >= 3 && $parts[0] > 0 && $parts[2] > $parts[1]) {
            $rows[$parts[0]] = [
                'item_id' => $parts[0],
                'min_amount' => $parts[1],
                'target_amount' => $parts[2],
            ];
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

        foreach (aa_ints($_POST['buff_items'] ?? []) as $id) {
            $ok = $ok && $con->query("INSERT INTO `aa_items` (`char_id`,`type`,`item_id`) VALUES ({$selected_char_id},0,{$id})");
        }
        foreach (aa_ints($_POST['pickup_items'] ?? []) as $id) {
            $ok = $ok && $con->query("INSERT INTO `aa_items` (`char_id`,`type`,`item_id`) VALUES ({$selected_char_id},2,{$id})");
        }
        foreach (aa_ints($_POST['keep_items'] ?? []) as $id) {
            $ok = $ok && $con->query("INSERT INTO `aa_items` (`char_id`,`type`,`item_id`) VALUES ({$selected_char_id},3,{$id})");
        }
        foreach (aa_buy_rows($_POST['buy_items'] ?? '') as $row) {
            $ok = $ok && $con->query("INSERT INTO `aa_items` (`char_id`,`type`,`item_id`,`min_hp`,`min_sp`) VALUES ({$selected_char_id},4,{$row['item_id']},{$row['min_amount']},{$row['target_amount']})");
        }

        foreach (aa_ints($_POST['attack_skills'] ?? []) as $skill_id) {
            $lv = max(1, min(20, (int)($_POST['skill_lv'][$skill_id] ?? 1)));
            $swarm = max(0, min(50, (int)($_POST['skill_swarm'][$skill_id] ?? 0)));
            $ok = $ok && $con->query("INSERT INTO `aa_skills` (`char_id`,`type`,`skill_id`,`skill_lv`,`min_hp`) VALUES ({$selected_char_id},2,{$skill_id},{$lv},{$swarm})");
        }

        if ($ok) {
            $con->query("COMMIT");
            $message = 'บันทึกแล้ว ถ้าตัวละครออนไลน์อยู่ให้ออกเข้าเกมใหม่ หรือปิดเปิด AI ใหม่เพื่อโหลดค่า';
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
    if ((int)$row->type === 4) $selected['buy'][] = "{$row->item_id},{$row->min_hp},{$row->min_sp}";
}
$res = $con->query("SELECT `skill_id`,`skill_lv`,`min_hp` FROM `aa_skills` WHERE `char_id`={$selected_char_id} AND `type`=2");
while ($res && ($row = $res->fetch_object())) {
    $selected['skills'][(int)$row->skill_id] = ['lv' => (int)$row->skill_lv, 'swarm' => (int)$row->min_hp];
}

$inventory = [];
$res = $con->query("SELECT `nameid`,SUM(`amount`) AS amount FROM `inventory` WHERE `char_id`={$selected_char_id} AND `nameid` > 0 GROUP BY `nameid` ORDER BY `nameid` LIMIT 120");
while ($res && ($row = $res->fetch_object())) {
    $inventory[] = ['id' => (int)$row->nameid, 'amount' => (int)$row->amount, 'name' => aa_name($con, (int)$row->nameid)];
}

$skills = [];
$res = $con->query("SELECT `id`,`lv` FROM `skill` WHERE `char_id`={$selected_char_id} AND `id` > 0 AND `lv` > 0 ORDER BY `id` LIMIT 120");
while ($res && ($row = $res->fetch_object())) {
    $skills[] = ['id' => (int)$row->id, 'lv' => (int)$row->lv, 'name' => aa_skill_name($con, (int)$row->id)];
}
?>
<section class="fxt-template-animation fxt-template-layout22" data-bg-image="<?= $app_url ?>/assets/img/figure/bg.jpg">
    <div class="star-animation"><div id="stars4"></div><div id="stars5"></div></div>
    <div class="container">
        <div class="row align-items-center justify-content-md-center shadow-lg">
            <div class="col-lg-12 fxt-bg-color">
                <div class="fxt-content">
                    <div class="fxt-form">
                        <h2>ตั้งค่า AI ฟาร์ม</h2>
                        <p>เลือกจากรายการได้เลย ไม่ต้องจำเลขไอเทมหรือเลขสกิล</p>
                        <?php require_once '../template/menu.php' ?>

                        <div class="shadow p-3 mb-3 bg-body rounded">
                            <?php if ($message) : ?><div class="alert alert-success"><?= $message ?></div><?php endif; ?>
                            <?php if ($error) : ?><div class="alert alert-warning"><?= $error ?></div><?php endif; ?>

                            <?php if (!empty($char_list)) : ?>
                                <form method="get" class="mb-3">
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
                                <form method="post">
                                    <input type="hidden" name="char_id" value="<?= (int)$selected_char_id ?>">

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
                                            <label class="form-label fw-bold">โอกาสใช้สกิลโจมตี %</label>
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

                                    <h5 class="mt-3">ของในตัว</h5>
                                    <p class="text-muted">รายการนี้อ่านจากตัวละครล่าสุด ถ้าออนไลน์อยู่แล้วไม่เห็นของใหม่ ให้ relog หรือกดโหลดใหม่หลังเซฟตัวละคร</p>
                                    <div class="row">
                                        <?php foreach ($inventory as $item) : ?>
                                            <div class="col-lg-6 mb-2">
                                                <div class="border rounded p-2">
                                                    <div class="fw-bold"><?= htmlspecialchars($item['name']) ?> <span class="badge bg-secondary">x<?= $item['amount'] ?></span></div>
                                                    <label class="me-2"><input type="checkbox" name="buff_items[]" value="<?= $item['id'] ?>" <?= isset($selected['buff'][$item['id']]) ? 'checked' : '' ?>> ไอเทมบัฟ</label>
                                                    <label class="me-2"><input type="checkbox" name="pickup_items[]" value="<?= $item['id'] ?>" <?= isset($selected['pickup'][$item['id']]) ? 'checked' : '' ?>> เก็บของนี้</label>
                                                    <label><input type="checkbox" name="keep_items[]" value="<?= $item['id'] ?>" <?= isset($selected['keep'][$item['id']]) ? 'checked' : '' ?>> ไม่ฝากคลัง</label>
                                                </div>
                                            </div>
                                        <?php endforeach; ?>
                                    </div>

                                    <h5 class="mt-3">สกิลโจมตี</h5>
                                    <?php foreach ($skills as $skill) : $saved = $selected['skills'][$skill['id']] ?? null; ?>
                                        <div class="border rounded p-2 mb-2">
                                            <label class="fw-bold">
                                                <input type="checkbox" name="attack_skills[]" value="<?= $skill['id'] ?>" <?= $saved ? 'checked' : '' ?>>
                                                <?= htmlspecialchars($skill['name']) ?> <span class="badge bg-secondary">Lv.<?= $skill['lv'] ?></span>
                                            </label>
                                            <div class="row mt-2">
                                                <div class="col-md-6"><input name="skill_lv[<?= $skill['id'] ?>]" class="form-control" type="number" min="1" max="<?= $skill['lv'] ?>" value="<?= $saved['lv'] ?? $skill['lv'] ?>"></div>
                                                <div class="col-md-6"><input name="skill_swarm[<?= $skill['id'] ?>]" class="form-control" type="number" min="0" max="50" value="<?= $saved['swarm'] ?? 0 ?>" placeholder="โดนรุมกี่ตัวถึงใช้"></div>
                                            </div>
                                        </div>
                                    <?php endforeach; ?>

                                    <div class="mb-3">
                                        <label class="form-label fw-bold">ซื้อของอัตโนมัติ</label>
                                        <textarea name="buy_items" class="form-control" rows="5" placeholder="501,20,100"><?= htmlspecialchars(implode("\n", $selected['buy'])) ?></textarea>
                                        <small class="text-muted">รูปแบบ: ไอดีไอเทม, เหลือน้อยกว่า, ซื้อให้ถึง เช่น 501,20,100</small>
                                    </div>

                                    <button name="save_ai" value="1" type="submit" class="btn btn-primary"><i class="fas fa-save"></i> บันทึกตั้งค่า AI</button>
                                    <button name="reset_ai" value="1" type="submit" class="btn btn-danger" onclick="return confirm('ล้างค่าทั้งหมด?')"><i class="fas fa-trash"></i> ล้างค่าทั้งหมด</button>
                                </form>
                            <?php endif; ?>
                        </div>
                    </div>
                    <div class="fxt-footer">
                        <p>คุณต้องการออกจากระบบใช่หรือไม่ ?<a href="<?= $app_url ?>/logout.php" class="switcher-text2 inline-text">คลิกที่นี้</a></p>
                    </div>
                </div>
            </div>
        </div>
    </div>
</section>
<?php require '../template/footer.php'; ?>
