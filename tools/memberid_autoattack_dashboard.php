<?php
/**
 * SafeZone Auto Attack dashboard bridge.
 *
 * Put this file in memberid/dashboard/autoattack.php on the website host.
 */
declare(strict_types=1);

require '../template/header.php';
require '../library/database.php';

if (!isset($_SESSION["session_user"]) || $_SESSION["session_user"] !== TRUE) {
    header("location: " . $app_url . "/login.php");
    exit;
}

const SAFEZONE_AUTOATTACK_URL = 'http://43.229.151.208:31910/autoattack';

function safezone_fetch_account(mysqli $db, int $accountId): array
{
    $stmt = $db->prepare('SELECT account_id, userid, web_auth_token FROM login WHERE account_id = ? LIMIT 1');
    $stmt->bind_param('i', $accountId);
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
    $accountId = (int)($_SESSION['account_id'] ?? 0);
    $account = safezone_fetch_account($con, $accountId);
    $characters = safezone_fetch_characters($con, (int)$account['account_id']);
    $error = '';
} catch (Throwable $e) {
    $account = [
        'account_id' => 0,
        'userid' => (string)($_SESSION['userid'] ?? ''),
        'web_auth_token' => '',
    ];
    $characters = [];
    $error = $e->getMessage();
}

$dashboardUrl = rtrim((string)$app_url, '/') . '/dashboard/index.php';
?>

<meta name="viewport" content="width=device-width, initial-scale=1.0">
<style>
    :root {
        --aa-bg: #090604;
        --aa-panel: rgba(22, 14, 9, .92);
        --aa-panel-soft: rgba(42, 27, 16, .72);
        --aa-line: rgba(233, 185, 95, .34);
        --aa-line-soft: rgba(255, 232, 174, .13);
        --aa-gold: #f4c56a;
        --aa-cream: #fff4cf;
        --aa-red: #e4573d;
        --aa-green: #64d184;
        --aa-blue: #75c7ff;
        --aa-muted: #b9a98b;
        --aa-shadow: 0 24px 80px rgba(0,0,0,.58);
    }

    html, body { min-height: 100%; }
    body {
        background:
            radial-gradient(circle at 12% 12%, rgba(219, 151, 53, .22), transparent 30%),
            radial-gradient(circle at 85% 3%, rgba(116, 69, 255, .17), transparent 28%),
            linear-gradient(135deg, #070402 0%, #170d07 44%, #050403 100%) !important;
        color: var(--aa-cream) !important;
        overflow-x: hidden;
    }

    .aa-dashboard {
        position: relative;
        z-index: 2;
        width: min(100% - 28px, 980px);
        margin: 0 auto;
        padding: 34px 0;
    }

    .aa-panel {
        border: 1px solid var(--aa-line);
        background: linear-gradient(145deg, var(--aa-panel), rgba(8,5,3,.9));
        border-radius: 24px;
        box-shadow: var(--aa-shadow), inset 0 1px 0 rgba(255,255,255,.06);
        overflow: hidden;
    }

    .aa-head {
        display: grid;
        grid-template-columns: 1fr auto;
        gap: 16px;
        align-items: center;
        padding: 24px;
        border-bottom: 1px solid var(--aa-line-soft);
    }

    .aa-title {
        margin: 0;
        color: var(--aa-gold);
        font-size: clamp(24px, 3vw, 36px);
        font-weight: 900;
    }

    .aa-desc {
        margin: 8px 0 0;
        color: var(--aa-muted);
        line-height: 1.75;
    }

    .aa-user {
        border: 1px solid rgba(117,199,255,.35);
        color: var(--aa-blue);
        background: rgba(117,199,255,.08);
        border-radius: 999px;
        padding: 10px 14px;
        font-weight: 800;
        white-space: nowrap;
    }

    .aa-body {
        padding: 24px;
        display: grid;
        gap: 18px;
    }

    .aa-alert {
        border-radius: 18px;
        padding: 16px;
        border: 1px solid rgba(228,87,61,.38);
        background: rgba(228,87,61,.12);
        color: #ffd7ce;
        font-weight: 700;
        line-height: 1.65;
    }

    .aa-alert.is-warning {
        border-color: rgba(244,197,106,.42);
        background: rgba(244,197,106,.1);
        color: var(--aa-cream);
    }

    .aa-field label {
        display: block;
        margin-bottom: 9px;
        color: var(--aa-gold);
        font-weight: 800;
    }

    .aa-select {
        width: 100%;
        min-height: 54px;
        border-radius: 16px;
        border: 1px solid rgba(233,185,95,.32);
        background: rgba(5,4,3,.72);
        color: var(--aa-cream);
        padding: 12px 14px;
        outline: 0;
        font-size: 16px;
    }

    .aa-select option { color: #111; }

    .aa-actions {
        display: grid;
        grid-template-columns: 1fr auto;
        gap: 12px;
        align-items: center;
    }

    .aa-primary,
    .aa-secondary {
        min-height: 52px;
        border-radius: 16px;
        padding: 12px 18px;
        font-weight: 900;
        text-decoration: none;
        text-align: center;
        border: 1px solid transparent;
        cursor: pointer;
    }

    .aa-primary {
        background: linear-gradient(135deg, #ffe7ab, #f4c56a 48%, #b9792d);
        color: #170b03;
        box-shadow: 0 16px 40px rgba(244,197,106,.18);
    }

    .aa-secondary {
        background: rgba(0,0,0,.24);
        border-color: rgba(233,185,95,.26);
        color: var(--aa-cream);
    }

    .aa-note {
        color: var(--aa-muted);
        line-height: 1.7;
        margin: 0;
    }

    @media (max-width: 720px) {
        .aa-dashboard { width: min(100% - 20px, 760px); padding: 18px 0; }
        .aa-head { grid-template-columns: 1fr; padding: 18px; }
        .aa-body { padding: 18px; }
        .aa-actions { grid-template-columns: 1fr; }
    }
</style>

<section class="fxt-template-animation fxt-template-layout22">
    <main class="aa-dashboard">
        <section class="aa-panel">
            <div class="aa-head">
                <div>
                    <h1 class="aa-title">ตั้งค่า AI ฟาร์ม</h1>
                    <p class="aa-desc">เลือกตัวละคร แล้วเปิดหน้าตั้งค่า ระบบจะส่งข้อมูลยืนยันให้อัตโนมัติ ไม่ต้องกรอก AID/GID/Token เอง</p>
                </div>
                <?php if (!empty($account['userid'])): ?>
                    <div class="aa-user"><?= htmlspecialchars((string)$account['userid'], ENT_QUOTES, 'UTF-8') ?></div>
                <?php endif; ?>
            </div>

            <div class="aa-body">
                <?php if ($error): ?>
                    <div class="aa-alert"><?= htmlspecialchars($error, ENT_QUOTES, 'UTF-8') ?></div>
                    <div class="aa-actions">
                        <a class="aa-secondary" href="<?= htmlspecialchars($dashboardUrl, ENT_QUOTES, 'UTF-8') ?>">กลับ Dashboard</a>
                    </div>
                <?php elseif (!$characters): ?>
                    <div class="aa-alert is-warning">ยังไม่มีตัวละครในบัญชีนี้</div>
                    <div class="aa-actions">
                        <a class="aa-secondary" href="<?= htmlspecialchars($dashboardUrl, ENT_QUOTES, 'UTF-8') ?>">กลับ Dashboard</a>
                    </div>
                <?php else: ?>
                    <div class="aa-field">
                        <label for="safezone-aa-char">ตัวละคร</label>
                        <select id="safezone-aa-char" class="aa-select">
                            <?php foreach ($characters as $char): ?>
                                <option value="<?= (int)$char['char_id'] ?>"><?= htmlspecialchars((string)$char['name'], ENT_QUOTES, 'UTF-8') ?></option>
                            <?php endforeach; ?>
                        </select>
                    </div>

                    <div class="aa-actions">
                        <button id="safezone-aa-open" type="button" class="aa-primary">เปิดหน้าตั้งค่า AI</button>
                        <a class="aa-secondary" href="<?= htmlspecialchars($dashboardUrl, ENT_QUOTES, 'UTF-8') ?>">กลับ Dashboard</a>
                    </div>

                    <p class="aa-note">หน้าตั้งค่า AI จะเปิดในแท็บใหม่ และใช้สิทธิ์ของบัญชีที่ล็อกอินอยู่เท่านั้น</p>
                <?php endif; ?>
            </div>
        </section>
    </main>
</section>

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

<?php require '../template/footer.php'; ?>
