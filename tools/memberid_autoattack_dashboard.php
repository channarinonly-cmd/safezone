<?php
/**
 * SafeZone Auto Attack dashboard bridge.
 *
 * Put this file in memberid/dashboard/autoattack.php on the website host.
 * It reuses the member website login session, finds the game account token, and
 * opens the simple Auto Attack UI without asking players for AID/GID/AuthToken.
 */
declare(strict_types=1);
session_start();

const SAFEZONE_AUTOATTACK_URL = 'http://43.229.151.208:31910/autoattack';

$configCandidates = [
    __DIR__ . '/../library/database.php',
    __DIR__ . '/../library/connect.php',
    __DIR__ . '/../library/config.php',
    __DIR__ . '/../config.php',
    __DIR__ . '/../../memberid/library/database.php',
    __DIR__ . '/../../library/connect.php',
];

foreach ($configCandidates as $file) {
    if (is_file($file)) {
        require_once $file;
        break;
    }
}

function safezone_member_db(): mysqli
{
    foreach (['conn', 'connect', 'mysqli', 'db', 'con'] as $name) {
        if (isset($GLOBALS[$name]) && $GLOBALS[$name] instanceof mysqli) {
            $GLOBALS[$name]->set_charset('utf8');
            return $GLOBALS[$name];
        }
    }

    throw new RuntimeException('ไม่พบการเชื่อมต่อฐานข้อมูลของเว็บสมาชิก');
}

function safezone_session_user(): string
{
    if (empty($_SESSION['session_user'])) {
        throw new RuntimeException('กรุณาเข้าสู่ระบบสมาชิกก่อน');
    }

    foreach (['userid', 'username', 'user', 'login', 'account'] as $key) {
        if (!empty($_SESSION[$key]) && is_string($_SESSION[$key])) {
            return $_SESSION[$key];
        }
    }

    throw new RuntimeException('กรุณาเข้าสู่ระบบสมาชิกก่อน');
}

function safezone_fetch_account(mysqli $db, string $userid): array
{
    $stmt = $db->prepare('SELECT account_id, web_auth_token FROM login WHERE userid = ? LIMIT 1');
    $stmt->bind_param('s', $userid);
    $stmt->execute();
    $account = $stmt->get_result()->fetch_assoc();
    $stmt->close();

    if (!$account) {
        throw new RuntimeException('ไม่พบบัญชีเกมของสมาชิกนี้');
    }

    if (empty($account['web_auth_token'])) {
        throw new RuntimeException('กรุณาเข้าเกมด้วยไอดีนี้ 1 ครั้ง แล้วกลับมาเปิดหน้านี้ใหม่');
    }

    return $account;
}

function safezone_fetch_characters(mysqli $db, int $accountId): array
{
    $stmt = $db->prepare('SELECT char_id, name FROM `char` WHERE account_id = ? ORDER BY char_num, char_id');
    $stmt->bind_param('i', $accountId);
    $stmt->execute();
    $result = $stmt->get_result();
    $characters = [];

    while ($row = $result->fetch_assoc()) {
        $characters[] = $row;
    }

    $stmt->close();
    return $characters;
}

try {
    $db = safezone_member_db();
    $userid = safezone_session_user();
    $account = safezone_fetch_account($db, $userid);
    $characters = safezone_fetch_characters($db, (int)$account['account_id']);
    $error = '';
} catch (Throwable $e) {
    $account = ['account_id' => 0, 'web_auth_token' => ''];
    $characters = [];
    $error = $e->getMessage();
}
?>
<!doctype html>
<html lang="th">
<head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>ตั้งค่า AI ฟาร์ม</title>
    <link rel="stylesheet" href="https://www.safezone-ro.com/memberid/assets/css/bootstrap.min.css">
    <link href="https://fonts.googleapis.com/css2?family=Sarabun:wght@300;400;600;700&display=swap" rel="stylesheet">
    <style>
        body{font-family:Sarabun,Tahoma,sans-serif;background:#f6f7fb;color:#20242a}
        .safezone-aa{max-width:760px;margin:28px auto;background:#fff;border:1px solid #e3e7ef;border-radius:10px;padding:24px}
        .safezone-aa h1{font-size:24px;font-weight:700;margin:0 0 8px}
        .safezone-aa p{color:#64748b;margin-bottom:18px}
        .safezone-aa .btn{font-weight:700}
        .safezone-aa .alert{margin-top:14px}
    </style>
</head>
<body>
<div class="safezone-aa">
    <h1>ตั้งค่า AI ฟาร์ม</h1>
    <p>เลือกตัวละคร แล้วกดเปิดหน้าตั้งค่า ระบบจะกรอกข้อมูลยืนยันให้เอง</p>

    <?php if ($error): ?>
        <div class="alert alert-danger"><?= htmlspecialchars($error, ENT_QUOTES, 'UTF-8') ?></div>
    <?php elseif (!$characters): ?>
        <div class="alert alert-warning">ยังไม่มีตัวละครในบัญชีนี้</div>
    <?php else: ?>
        <div class="mb-3">
            <label class="form-label">ตัวละคร</label>
            <select id="safezone-aa-char" class="form-select">
                <?php foreach ($characters as $char): ?>
                    <option value="<?= (int)$char['char_id'] ?>"><?= htmlspecialchars($char['name'], ENT_QUOTES, 'UTF-8') ?></option>
                <?php endforeach; ?>
            </select>
        </div>
        <button id="safezone-aa-open" type="button" class="btn btn-primary">เปิดหน้าตั้งค่า AI</button>
    <?php endif; ?>
</div>

<script>
const accountId = <?= (int)$account['account_id'] ?>;
const token = <?= json_encode((string)$account['web_auth_token']) ?>;
const baseUrl = <?= json_encode(SAFEZONE_AUTOATTACK_URL) ?>;
const openButton = document.getElementById("safezone-aa-open");
if (openButton) {
    openButton.addEventListener("click", () => {
        const charId = document.getElementById("safezone-aa-char").value;
        const url = `${baseUrl}?aid=${encodeURIComponent(accountId)}&gid=${encodeURIComponent(charId)}&token=${encodeURIComponent(token)}`;
        window.open(url, "_blank", "noopener");
    });
}
</script>
</body>
</html>
