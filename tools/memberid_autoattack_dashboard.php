<?php
require '../template/header.php';
require '../library/database.php';

if (!isset($_SESSION["session_user"]) || $_SESSION["session_user"] !== TRUE) {
    header("location: " . $app_url . "/login.php");
    exit;
}

$account_id = (int)$_SESSION['account_id'];
$endpoint = 'http://43.229.151.208:31910';
$message = '';
$error = '';

function safezone_aa_post($url, array $fields)
{
    if (function_exists('curl_init')) {
        $ch = curl_init($url);
        curl_setopt_array($ch, [
            CURLOPT_POST => true,
            CURLOPT_POSTFIELDS => $fields,
            CURLOPT_RETURNTRANSFER => true,
            CURLOPT_CONNECTTIMEOUT => 5,
            CURLOPT_TIMEOUT => 10,
        ]);
        $body = curl_exec($ch);
        $code = (int)curl_getinfo($ch, CURLINFO_HTTP_CODE);
        $err = curl_error($ch);
        curl_close($ch);
        return [$code, $body === false ? $err : $body];
    }

    $boundary = '----safezone-aa-' . md5((string)microtime(true));
    $body = '';
    foreach ($fields as $name => $value) {
        $body .= "--{$boundary}\r\n";
        $body .= 'Content-Disposition: form-data; name="' . addslashes((string)$name) . '"' . "\r\n\r\n";
        $body .= (string)$value . "\r\n";
    }
    $body .= "--{$boundary}--\r\n";

    $context = stream_context_create([
        'http' => [
            'method' => 'POST',
            'header' => "Content-Type: multipart/form-data; boundary={$boundary}\r\n",
            'content' => $body,
            'timeout' => 10,
        ],
    ]);
    $body = @file_get_contents($url, false, $context);
    $code = 0;
    if (!empty($http_response_header[0]) && preg_match('/\s(\d{3})\s/', $http_response_header[0], $m)) {
        $code = (int)$m[1];
    }
    return [$code, $body === false ? 'เชื่อมต่อระบบ AI ไม่ได้' : $body];
}

function safezone_aa_lines_to_ids($text)
{
    preg_match_all('/\d+/', (string)$text, $matches);
    return array_values(array_unique(array_map('intval', $matches[0])));
}

function safezone_aa_buy_rows($text)
{
    $rows = [];
    foreach (preg_split('/\r\n|\r|\n/', (string)$text) as $line) {
        preg_match_all('/\d+/', $line, $matches);
        $parts = array_map('intval', $matches[0]);
        if (count($parts) >= 3 && $parts[0] > 0 && $parts[2] > $parts[1]) {
            $rows[] = [
                'item_id' => $parts[0],
                'min_amount' => $parts[1],
                'target_amount' => $parts[2],
            ];
        }
    }
    return $rows;
}

$account = $con->query("SELECT account_id, web_auth_token FROM login WHERE account_id={$account_id} LIMIT 1")->fetch_object();
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

$config = [
    'buff_items' => [],
    'keep_items' => [],
    'buy_items' => [],
];

if (!$account || empty($account->web_auth_token)) {
    $error = 'กรุณาเข้าเกมด้วยไอดีนี้ 1 ครั้ง แล้วกลับมาเปิดหน้านี้ใหม่';
} elseif (empty($char_list)) {
    $error = 'ยังไม่มีตัวละครในบัญชีนี้';
} elseif (!$owns_char) {
    $error = 'ตัวละครนี้ไม่ตรงกับบัญชีของคุณ';
} else {
    $auth = [
        'AID' => $account_id,
        'GID' => $selected_char_id,
        'AuthToken' => $account->web_auth_token,
    ];

    if ($_SERVER['REQUEST_METHOD'] === 'POST' && isset($_POST['save_ai'])) {
        $payload = [
            'buff_items' => safezone_aa_lines_to_ids($_POST['buff_items'] ?? ''),
            'keep_items' => safezone_aa_lines_to_ids($_POST['keep_items'] ?? ''),
            'buy_items' => safezone_aa_buy_rows($_POST['buy_items'] ?? ''),
        ];
        [$code, $body] = safezone_aa_post($endpoint . '/autoattack/config/save', $auth + ['data' => json_encode($payload)]);
        if ($code === 200) {
            $message = 'บันทึกแล้ว เข้าเกมใหม่หรือรอระบบโหลดค่าตัวละครอีกครั้ง';
        } else {
            $error = 'บันทึกไม่สำเร็จ: ' . htmlspecialchars((string)$body);
        }
    }

    [$code, $body] = safezone_aa_post($endpoint . '/autoattack/config/load', $auth);
    if ($code === 200) {
        $loaded = json_decode((string)$body, true);
        if (is_array($loaded)) {
            $config = $loaded + $config;
        }
    } elseif (!$error) {
        $error = 'โหลดค่า AI ไม่สำเร็จ';
    }
}
?>
<section class="fxt-template-animation fxt-template-layout22" data-bg-image="<?= $app_url ?>/assets/img/figure/bg.jpg">
    <div class="star-animation">
        <div id="stars4"></div>
        <div id="stars5"></div>
    </div>
    <div class="container">
        <div class="row align-items-center justify-content-md-center shadow-lg">
            <div class="col-lg-12 fxt-bg-color">
                <div class="fxt-content">
                    <div class="fxt-form">
                        <h2>ตั้งค่า AI ฟาร์ม</h2>
                        <p>เลือกตัวละคร แล้วตั้งค่าของบัฟ ของที่ไม่ฝากคลัง และของที่ให้ซื้ออัตโนมัติ</p>
                        <?php require_once '../template/menu.php' ?>

                        <div class="shadow p-3 mb-3 bg-body rounded">
                            <?php if ($message) : ?>
                                <div class="alert alert-success"><?= $message ?></div>
                            <?php endif; ?>
                            <?php if ($error) : ?>
                                <div class="alert alert-warning"><?= $error ?></div>
                            <?php endif; ?>

                            <?php if ($account && !empty($account->web_auth_token) && !empty($char_list)) : ?>
                                <form method="get" class="mb-3">
                                    <label class="form-label fw-bold">ตัวละคร</label>
                                    <div class="input-group">
                                        <select name="char_id" class="form-select">
                                            <?php foreach ($char_list as $char) : ?>
                                                <option value="<?= (int)$char->char_id ?>" <?= (int)$char->char_id === $selected_char_id ? 'selected' : '' ?>>
                                                    <?= htmlspecialchars($char->name) ?>
                                                </option>
                                            <?php endforeach; ?>
                                        </select>
                                        <button class="btn btn-dark" type="submit">โหลดค่า</button>
                                    </div>
                                </form>

                                <?php if ($owns_char) : ?>
                                    <form method="post">
                                        <input type="hidden" name="char_id" value="<?= (int)$selected_char_id ?>">
                                        <div class="mb-3">
                                            <label class="form-label fw-bold">ไอเทมบัฟ</label>
                                            <textarea name="buff_items" class="form-control" rows="5" placeholder="ใส่ไอดีไอเทม บรรทัดละ 1 รายการ"><?= htmlspecialchars(implode("\n", $config['buff_items'] ?? [])) ?></textarea>
                                            <small class="text-muted">ระบบเกมจะรับเฉพาะรายการที่อยู่ใน ai_item_buff.txt</small>
                                        </div>
                                        <div class="mb-3">
                                            <label class="form-label fw-bold">ของที่ไม่ฝากเข้าคลัง</label>
                                            <textarea name="keep_items" class="form-control" rows="5" placeholder="ใส่ไอดีไอเทม บรรทัดละ 1 รายการ"><?= htmlspecialchars(implode("\n", $config['keep_items'] ?? [])) ?></textarea>
                                        </div>
                                        <div class="mb-3">
                                            <label class="form-label fw-bold">ซื้อของอัตโนมัติ</label>
                                            <textarea name="buy_items" class="form-control" rows="5" placeholder="ตัวอย่าง: 501,20,100"><?php
                                                $buy_lines = [];
                                                foreach (($config['buy_items'] ?? []) as $row) {
                                                    $buy_lines[] = (int)$row['item_id'] . ',' . (int)$row['min_amount'] . ',' . (int)$row['target_amount'];
                                                }
                                                echo htmlspecialchars(implode("\n", $buy_lines));
                                            ?></textarea>
                                            <small class="text-muted">รูปแบบ: ไอดีไอเทม, เหลือน้อยกว่า, ซื้อให้ถึง เช่น 501,20,100</small>
                                        </div>
                                        <button name="save_ai" value="1" type="submit" class="btn btn-primary">
                                            <i class="fas fa-save"></i> บันทึกตั้งค่า AI
                                        </button>
                                    </form>
                                <?php endif; ?>
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
<?php
require '../template/footer.php';
?>
