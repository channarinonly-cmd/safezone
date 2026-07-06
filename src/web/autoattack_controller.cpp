// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "autoattack_controller.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include <common/showmsg.hpp>
#include <common/sql.hpp>

#include "auth.hpp"
#include "sqllock.hpp"
#include "web.hpp"

static bool autoattack_check_character(int account_id, int char_id)
{
	SQLLock sl(CHAR_SQL_LOCK);
	sl.lock();
	Sql* handle = sl.getHandle();
	SqlStmt* stmt = SqlStmt_Malloc(handle);
	bool ok = false;

	if (SQL_SUCCESS == SqlStmt_Prepare(stmt,
		"SELECT `char_id` FROM `%s` WHERE `account_id` = ? AND `char_id` = ? LIMIT 1",
		char_db_table)
		&& SQL_SUCCESS == SqlStmt_BindParam(stmt, 0, SQLDT_INT, &account_id, sizeof(account_id))
		&& SQL_SUCCESS == SqlStmt_BindParam(stmt, 1, SQLDT_INT, &char_id, sizeof(char_id))
		&& SQL_SUCCESS == SqlStmt_Execute(stmt)
	) {
		ok = SqlStmt_NumRows(stmt) > 0;
	} else {
		SqlStmt_ShowDebug(stmt);
	}

	SqlStmt_Free(stmt);
	sl.unlock();
	return ok;
}

static std::unordered_set<int> autoattack_read_buff_item_ids()
{
	std::unordered_set<int> ids;
	std::ifstream file("db/custom/ai_item_buff.txt");
	std::string line;

	while (std::getline(file, line)) {
		if (line.empty() || line[0] == '/')
			continue;

		std::stringstream ss(line);
		std::string field;
		if (!std::getline(ss, field, ','))
			continue;

		try {
			int item_id = std::stoi(field);
			if (item_id > 0)
				ids.insert(item_id);
		} catch (...) {
		}
	}

	return ids;
}

static bool autoattack_require_auth(const Request& req, Response& res, int& account_id, int& char_id)
{
	if (!req.has_file("AID") || !req.has_file("GID") || !isAuthorized(req, false)) {
		res.status = HTTP_BAD_REQUEST;
		res.set_content("ไม่ได้รับอนุญาต", "text/plain; charset=utf-8");
		return false;
	}

	account_id = std::stoi(req.get_file_value("AID").content);
	char_id = std::stoi(req.get_file_value("GID").content);

	if (!autoattack_check_character(account_id, char_id)) {
		res.status = HTTP_BAD_REQUEST;
		res.set_content("ตัวละครนี้ไม่ตรงกับบัญชี", "text/plain; charset=utf-8");
		return false;
	}

	return true;
}

static bool autoattack_ensure_status_table(Sql* handle)
{
	if (!handle)
		return false;

	if (SQL_ERROR == Sql_Query(handle,
		"CREATE TABLE IF NOT EXISTS `aa_runtime_status` ("
		"`char_id` INT(11) NOT NULL,"
		"`account_id` INT(11) NOT NULL,"
		"`char_name` VARCHAR(24) NOT NULL DEFAULT '',"
		"`ai_active` TINYINT(1) NOT NULL DEFAULT 0,"
		"`state` VARCHAR(64) NOT NULL DEFAULT '',"
		"`map` VARCHAR(32) NOT NULL DEFAULT '',"
		"`x` SMALLINT(6) NOT NULL DEFAULT 0,"
		"`y` SMALLINT(6) NOT NULL DEFAULT 0,"
		"`hp` INT(11) NOT NULL DEFAULT 0,"
		"`max_hp` INT(11) NOT NULL DEFAULT 0,"
		"`sp` INT(11) NOT NULL DEFAULT 0,"
		"`max_sp` INT(11) NOT NULL DEFAULT 0,"
		"`weight` INT(11) NOT NULL DEFAULT 0,"
		"`max_weight` INT(11) NOT NULL DEFAULT 0,"
		"`zeny` INT(11) NOT NULL DEFAULT 0,"
		"`cash` INT(11) NOT NULL DEFAULT 0,"
		"`target_id` INT(11) NOT NULL DEFAULT 0,"
		"`target_name` VARCHAR(64) NOT NULL DEFAULT '',"
		"`pickup_item_id` INT(11) NOT NULL DEFAULT 0,"
		"`pickup_item_name` VARCHAR(64) NOT NULL DEFAULT '',"
		"`base_exp` BIGINT UNSIGNED NOT NULL DEFAULT 0,"
		"`max_base_exp` BIGINT UNSIGNED NOT NULL DEFAULT 0,"
		"`job_exp` BIGINT UNSIGNED NOT NULL DEFAULT 0,"
		"`max_job_exp` BIGINT UNSIGNED NOT NULL DEFAULT 0,"
		"`command` VARCHAR(32) NOT NULL DEFAULT '',"
		"`updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,"
		"PRIMARY KEY (`char_id`),"
		"KEY `account_id` (`account_id`)"
		") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4")
	) {
		Sql_ShowDebug(handle);
		return false;
	}

	return true;
}

HANDLER_FUNC(autoattack_page)
{
	static const char* html = R"HTML(
<!doctype html>
<html lang="th">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>สถานะ AI</title>
<style>
*{box-sizing:border-box}body{margin:0;font-family:Tahoma,Arial,sans-serif;background:#f0f2f5;color:#1e293b}
header{background:#0f172a;color:#fff;padding:16px 20px;box-shadow:0 2px 4px rgba(0,0,0,0.1);}main{max-width:1080px;margin:0 auto;padding:20px}
h1{font-size:22px;margin:0}h2{font-size:18px;margin:0;color:#0f172a;font-weight:bold;}.muted{color:#94a3b8;font-size:12px}
.panel{background:#fff;border:1px solid #e2e8f0;border-radius:12px;padding:20px;margin-bottom:16px;box-shadow:0 1px 3px rgba(0,0,0,0.05);}
.statusTop{display:grid;grid-template-columns:1fr;gap:12px}@media(min-width:760px){.statusTop{grid-template-columns:1.3fr 1fr 1fr}}
.badge{display:inline-flex;align-items:center;gap:6px;border-radius:999px;padding:6px 12px;font-weight:bold;font-size:13px;background:#f1f5f9;color:#475569}
.badge.on{background:#dcfce7;color:#166534;border:1px solid #bbf7d0;}.badge.off{background:#fee2e2;color:#991b1b;border:1px solid #fecaca;}
.big{font-size:26px;font-weight:800;margin:8px 0 4px;color:#0f172a;}.grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}
@media(min-width:760px){.grid{grid-template-columns:repeat(4,1fr)}}
.box{border:1px solid #e2e8f0;border-radius:10px;padding:12px;background:#f8fafc}.label{font-size:12px;color:#64748b;font-weight:bold;text-transform:uppercase;}.value{font-size:18px;font-weight:bold;margin-top:4px;word-break:break-word;color:#0f172a;}
.bar{height:6px;background:#e2e8f0;border-radius:999px;overflow:hidden;margin-top:8px}.bar span{display:block;height:100%;background:#3b82f6;width:0;transition:width 0.3s ease;}
.bar.hp span{background:#10b981;}.bar.weight span{background:#f59e0b;}.bar.weight.high span{background:#ef4444;}
.bar.exp span{background:#facc15;} .bar.jexp span{background:#c084fc;}
button{border:0;border-radius:8px;padding:10px 16px;background:#3b82f6;color:#fff;font-weight:bold;cursor:pointer;transition:background 0.2s;}button:hover{background:#2563eb;}
.btn-wrap {display:flex; gap:10px; margin-top:14px; flex-wrap:wrap;} .btn-wrap button {flex:1; padding:12px; font-size:14px;}
.btn-storage {background:#f59e0b;} .btn-storage:hover {background:#d97706;}
.btn-buy {background:#8b5cf6;} .btn-buy:hover {background:#7c3aed;}
.auth{display:none;}
.msg{font-size:14px;margin-top:10px;color:#059669;font-weight:bold;padding:10px;border-radius:8px;background:#ecfdf5;display:none;text-align:center;}
.msg.err{color:#dc2626;background:#fef2f2;}
.item-header {display:flex; justify-content:space-between; align-items:center; margin-bottom:16px; flex-wrap:wrap; gap:10px;}
.add-item-box {display:flex; gap:8px;} .add-item-box input {padding:8px 12px; border:1px solid #cbd5e1; border-radius:6px; font-size:14px; width:180px; outline:none;}
.item-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(190px, 1fr)); gap: 14px; }
.item-card { background: #fff; border: 1px solid #e2e8f0; border-radius: 10px; padding: 14px; text-align: center; box-shadow: 0 2px 4px rgba(0,0,0,0.02); transition:transform 0.2s, box-shadow 0.2s;}
.item-card:hover { transform:translateY(-2px); box-shadow: 0 4px 6px rgba(0,0,0,0.05); border-color:#cbd5e1;}
.item-card img { width: 48px; height: 48px; object-fit: contain; margin-bottom: 10px; filter: drop-shadow(0 2px 3px rgba(0,0,0,0.15)); }
.item-card .item-id { font-size: 14px; font-weight: 700; color: #1e293b; margin-bottom: 12px; background:#f1f5f9; display:inline-block; padding:2px 8px; border-radius:4px;}
.item-opts { display: flex; flex-direction: column; gap: 8px; text-align: left; }
.item-opts label { font-size: 13px; color: #334155; display: flex; align-items: center; gap: 8px; cursor: pointer; margin:0; user-select:none;}
.item-opts input[type="checkbox"] { width: 16px; height: 16px; margin:0; cursor: pointer; accent-color: #3b82f6; }
.buy-opts { display: flex; gap: 6px; margin-top: 10px; padding-top:10px; border-top:1px dashed #e2e8f0;}
.buy-opts input { width: 100%; padding: 6px; font-size: 12px; border: 1px solid #cbd5e1; border-radius: 4px; text-align: center; outline:none;}
.buy-opts input:focus {border-color:#3b82f6;}
</style>
</head>
<body>
<header>
	<div style="max-width:1080px; margin:0 auto; display:flex; justify-content:space-between; align-items:center;">
		<div><h1>ระบบ AI ฟาร์ม</h1><div class="muted">จัดการบอทและตั้งค่าไอเทมแบบเรียลไทม์</div></div>
		<button id="save" style="background:#10b981;">💾 บันทึกการตั้งค่า</button>
	</div>
</header>
<main>
	<div id="message" class="msg"></div>
	<section class="panel">
		<div class="statusTop">
			<div>
				<span id="onlineBadge" class="badge off">ยังไม่มีข้อมูล</span>
				<div id="stateText" class="big">รอข้อมูล AI</div>
				<div class="muted" id="whereText">เปิด AI ในเกมก่อน แล้วหน้านี้จะเริ่มอัปเดตเอง</div>
			</div>
			<div class="box"><div class="label">เป้าหมายปัจจุบัน</div><div class="value" id="targetText">-</div></div>
			<div class="box"><div class="label">กำลังเก็บไอเทม</div><div class="value" id="pickupText">-</div></div>
		</div>
		<div class="grid" style="margin-top:14px">
			<div class="box"><div class="label">HP</div><div class="value" id="hpText">0 / 0</div><div class="bar hp"><span id="hpBar"></span></div></div>
			<div class="box"><div class="label">SP</div><div class="value" id="spText">0 / 0</div><div class="bar"><span id="spBar"></span></div></div>
			<div class="box"><div class="label">Base EXP</div><div class="value" id="baseExpText" style="font-size:15px;">0 / 0</div><div class="bar exp"><span id="baseExpBar"></span></div></div>
			<div class="box"><div class="label">Job EXP</div><div class="value" id="jobExpText" style="font-size:15px;">0 / 0</div><div class="bar jexp"><span id="jobExpBar"></span></div></div>
			<div class="box"><div class="label">น้ำหนัก (Weight)</div><div class="value" id="weightText">0%</div><div class="bar weight" id="wBarWrap"><span id="weightBar"></span></div></div>
			<div class="box"><div class="label">การเงิน</div><div class="value" id="moneyText">0 Z / 0 CC</div></div>
			<div class="box" style="grid-column: span 2;"><div class="label">เวลา Stamina คงเหลือ</div><div class="value" id="staminaText" style="color:#eab308; font-size:22px;">0 นาที 0 วินาที</div></div>
		</div>
		<div class="btn-wrap">
			<button class="btn-storage" onclick="sendCommand('storage')">📦 สั่งบอทกลับเมืองไปฝากของ</button>
			<button class="btn-buy" onclick="sendCommand('buy')">🛒 สั่งบอทกลับเมืองไปซื้อของ</button>
		</div>
		<div class="muted" id="lastText" style="margin-top:12px; text-align:right;">อัปเดตล่าสุด: -</div>
	</section>

	<section class="panel">
		<div class="item-header">
			<div>
				<h2>📦 ตั้งค่าไอเทม (Inventory & Config)</h2>
				<div class="muted" style="margin-top:4px;">ติ๊กเลือกการทำงานให้ไอเทมแต่ละชิ้นได้เลย (รายชื่อดึงจากกระเป๋าตัวละคร)</div>
			</div>
			<div class="add-item-box">
				<input type="number" id="newItemId" placeholder="ใส่ ID ไอเทมเพิ่มเอง">
				<button type="button" onclick="addNewItem()">+ เพิ่ม</button>
			</div>
		</div>
		<div class="item-grid" id="itemGrid"></div>
	</section>
</main>
<script>
const $=id=>document.getElementById(id);
const msg=(t,bad=false)=>{
	const el=$("message"); el.textContent=t; el.className=bad?"msg err":"msg"; el.style.display="block";
	setTimeout(()=>el.style.display="none", 4000);
};

const p=new URLSearchParams(location.search);
const f=()=>{
	const x=new FormData();
	x.append("AID", p.get("aid")||"");
	x.append("GID", p.get("gid")||"");
	x.append("AuthToken", p.get("token")||"");
	return x;
};

const pct=(a,b)=>b>0?Math.max(0,Math.min(100,Math.round(a*100/b))):0;
const stateName=s=>({attack:"โจมตีมอนสเตอร์",pickup:"กำลังเก็บของ",searching:"กำลังหาเป้าหมาย",resting:"นั่งฟื้น HP/SP",teleport:"วิงหาเป้าหมาย",danger:"หนี/มอนรุม",dead:"ตาย / รอฟื้น",storage:"ฝากของเข้าคลัง",buy:"ซื้อของ / เปิดกล่อง"}[s]||s||"รอข้อมูล AI");

let allItems = new Set();
let buyData = {};
let itemAmounts = {};

async function loadConfig(){
	try {
		const r = await fetch("/autoattack/config/load", {method:"POST", body:f()});
		if (!r.ok) throw new Error(await r.text());
		const d = await r.json();

		allItems.clear();
		buyData = {};
		itemAmounts = {};
		$("itemGrid").innerHTML = "";

		(d.inventory || []).forEach(item => {
			allItems.add(item.id);
			itemAmounts[item.id] = item.amount;
		});

		(d.buff_items || []).forEach(id => allItems.add(id));
		(d.keep_items || []).forEach(id => allItems.add(id));
		(d.buy_items || []).forEach(b => {
			allItems.add(b.item_id);
			buyData[b.item_id] = { min: b.min_amount, target: b.target_amount };
		});

		Array.from(allItems).sort((a,b)=>a-b).forEach(id => {
			renderItemCard(id, 
				(d.buff_items || []).includes(id), 
				(d.keep_items || []).includes(id),
				itemAmounts[id] || 0
			);
		});
	} catch(e) {
		msg(e.message||"โหลดการตั้งค่าไม่สำเร็จ", true);
	}
}

async function refreshInventory(){
	try {
		const r = await fetch("/autoattack/config/load", {method:"POST", body:f()});
		if(!r.ok) return;
		const d = await r.json();
		
		let newAmts = {};
		(d.inventory || []).forEach(item => newAmts[item.id] = item.amount);
		
		document.querySelectorAll(".item-card").forEach(card => {
			const id = parseInt(card.id.replace("card-", ""));
			const amt = newAmts[id] || 0;
			const amtDiv = card.querySelector(".amt-text");
			
			if(amtDiv) {
				amtDiv.textContent = amt > 0 ? `มีอยู่: ${amt.toLocaleString()} ชิ้น` : `ไม่มีของในตัว`;
				amtDiv.style.color = amt > 0 ? "#10b981" : "#94a3b8";
			}
		});
	} catch(e){}
}

function renderItemCard(id, isBuff, isKeep, amount) {
	if($("card-"+id)) return;
	const isBuy = !!buyData[id];
	const minA = isBuy ? buyData[id].min : '';
	const maxA = isBuy ? buyData[id].target : '';
	
	const amountColor = amount > 0 ? "#10b981" : "#94a3b8";
	const amountText = amount > 0 ? `มีอยู่: ${amount.toLocaleString()} ชิ้น` : `ไม่มีของในตัว`;

	const div = document.createElement("div");
	div.className = "item-card";
	div.id = "card-"+id;
	div.innerHTML = `
		<img src="https://static.divine-pride.net/images/items/item/${id}.png" onerror="this.src='https://static.divine-pride.net/images/items/item/512.png'">
		<div><span class="item-id">ID: ${id}</span></div>
		<div class="amt-text" style="font-size:13px; font-weight:bold; color:${amountColor}; margin-bottom:8px;">${amountText}</div>
		<div class="item-opts">
			<label><input type="checkbox" class="cb-buff" value="${id}" ${isBuff ? 'checked':''}> ไอเทมบัฟ</label>
			<label><input type="checkbox" class="cb-keep" value="${id}" ${isKeep ? 'checked':''}> ห้ามฝากคลัง</label>
			<label><input type="checkbox" class="cb-buy" value="${id}" onchange="toggleBuy(${id})" ${isBuy ? 'checked':''}> ซื้ออัตโนมัติ</label>
		</div>
		<div class="buy-opts" id="buy-opt-${id}" style="display:${isBuy ? 'flex':'none'};">
			<input type="number" class="in-min" placeholder="เหลือน้อยกว่า" value="${minA}">
			<input type="number" class="in-max" placeholder="ซื้อให้ถึง" value="${maxA}">
		</div>
	`;
	$("itemGrid").appendChild(div);
}

function toggleBuy(id) {
	const cb = document.querySelector(`#card-${id} .cb-buy`).checked;
	document.getElementById(`buy-opt-${id}`).style.display = cb ? 'flex' : 'none';
}

function addNewItem() {
	const id = parseInt($("newItemId").value);
	if(id > 0) {
		if(!allItems.has(id)) {
			allItems.add(id);
			renderItemCard(id, false, false, 0);
		}
		$("newItemId").value = "";
	}
}

async function saveConfig(){
	try {
		const buff_items = [];
		const keep_items = [];
		const buy_items = [];

		document.querySelectorAll(".item-card").forEach(card => {
			const id = parseInt(card.id.replace("card-", ""));
			if(card.querySelector(".cb-buff").checked) buff_items.push(id);
			if(card.querySelector(".cb-keep").checked) keep_items.push(id);
			if(card.querySelector(".cb-buy").checked) {
				const minA = parseInt(card.querySelector(".in-min").value) || 0;
				const maxA = parseInt(card.querySelector(".in-max").value) || 0;
				if(maxA > minA) {
					buy_items.push({ item_id: id, min_amount: minA, target_amount: maxA });
				}
			}
		});

		const x = f();
		x.append("data", JSON.stringify({
			buff_items: buff_items,
			keep_items: keep_items,
			buy_items: buy_items
		}));

		$("save").textContent = "⏳ กำลังบันทึก...";
		const r = await fetch("/autoattack/config/save", {method:"POST", body:x});
		if(!r.ok) throw new Error(await r.text());
		$("save").textContent = "💾 บันทึกการตั้งค่า";
		msg("บันทึกการตั้งค่าไอเทมสำเร็จแล้ว!");
	} catch(e) {
		$("save").textContent = "💾 บันทึกการตั้งค่า";
		msg(e.message || "บันทึกไม่สำเร็จ", true);
	}
}

async function sendCommand(cmd) {
	try {
		const x = f();
		x.append("cmd", cmd);
		const r = await fetch("/autoattack/status", {method:"POST", body:x});
		if(r.ok) {
			msg(cmd === 'storage' ? "ส่งคำสั่งกลับไปฝากของแล้ว บอทจะวิงกลับเมืองทันที!" : "ส่งคำสั่งกลับไปซื้อของแล้ว บอทจะวิงกลับเมืองทันที!");
		}
	} catch(e) {
		msg("ส่งคำสั่งไม่สำเร็จ กรุณาลองใหม่", true);
	}
}

async function loadStatus(){
	try {
		const r=await fetch("/autoattack/status",{method:"POST",body:f()});
		if(!r.ok) throw new Error(await r.text());
		const d=await r.json();
		
		if(!d.has_data){
			$("onlineBadge").textContent="ยังไม่มีข้อมูล"; $("onlineBadge").className="badge off"; return;
		}

		const online = !!d.online && !!d.ai_active;
		$("onlineBadge").textContent=online?"AI กำลังทำงาน":"หยุดทำงาน / ออฟไลน์";
		$("onlineBadge").className=online?"badge on":"badge off";
		$("stateText").textContent=online?stateName(d.state):"AI เงียบหรือปิดอยู่";
		$("whereText").textContent=`${d.char_name||""} @ ${d.map||"-"} (${d.x||0}, ${d.y||0})`;
		
		$("targetText").textContent=d.target_name?`${d.target_name} (${d.target_id})`:"-";
		$("pickupText").textContent=d.pickup_item_name?`${d.pickup_item_name} (${d.pickup_item_id})`:"-";
		
		$("hpText").textContent=`${(d.hp||0).toLocaleString()} / ${(d.max_hp||0).toLocaleString()}`;
		$("spText").textContent=`${(d.sp||0).toLocaleString()} / ${(d.max_sp||0).toLocaleString()}`;
		$("hpBar").style.width=pct(d.hp,d.max_hp)+"%";
		$("spBar").style.width=pct(d.sp,d.max_sp)+"%";

		$("baseExpText").textContent=`${(d.base_exp||0).toLocaleString()} / ${(d.max_base_exp||0).toLocaleString()}`;
		$("baseExpBar").style.width=pct(d.base_exp,d.max_base_exp)+"%";
		$("jobExpText").textContent=`${(d.job_exp||0).toLocaleString()} / ${(d.max_job_exp||0).toLocaleString()}`;
		$("jobExpBar").style.width=pct(d.job_exp,d.max_job_exp)+"%";
		
		const st_m = Math.floor((d.stamina||0) / 60);
		const st_s = (d.stamina||0) % 60;
		$("staminaText").textContent=`${st_m} นาที ${st_s} วินาที`;
		
		const wPct = pct(d.weight, d.max_weight);
		$("weightText").textContent=`${wPct}%`;
		$("wBarWrap").className = wPct >= 90 ? "bar weight high" : "bar weight";
		
		$("moneyText").textContent=`${(d.zeny||0).toLocaleString()} Z / ${(d.cash||0).toLocaleString()} CC`;
		$("lastText").textContent=`อัปเดตล่าสุด: ${d.age||0} วินาทีที่แล้ว`;
	} catch(e){}
}

$("save").onclick=saveConfig;
if(p.has("aid")&&p.has("gid")&&p.has("token")){ loadConfig(); loadStatus(); }

setInterval(loadStatus, 2500);
setInterval(refreshInventory, 3000);
</script>
</body>
</html>
)HTML";

	res.set_content(html, "text/html; charset=utf-8");
}

HANDLER_FUNC(autoattack_config_load)
{
	int account_id = 0, char_id = 0;
	if (!autoattack_require_auth(req, res, account_id, char_id))
		return;

	nlohmann::json response;
	response["buff_items"] = nlohmann::json::array();
	response["keep_items"] = nlohmann::json::array();
	response["buy_items"] = nlohmann::json::array();
	response["inventory"] = nlohmann::json::array();

	SQLLock sl(MAP_SQL_LOCK);
	sl.lock();
	Sql* handle = sl.getHandle();
	if (!autoattack_ensure_status_table(handle)) {
		sl.unlock();
		res.set_content(response.dump(), "application/json");
		return;
	}

	if (SQL_ERROR == Sql_Query(handle,
		"SELECT `type`,`item_id`,`min_hp`,`min_sp` FROM `aa_items` WHERE `char_id` = %d AND `type` IN (0,3,4)",
		char_id)
	) {
		Sql_ShowDebug(handle);
		sl.unlock();
		res.status = HTTP_BAD_REQUEST;
		res.set_content("โหลดข้อมูลไม่ได้", "text/plain; charset=utf-8");
		return;
	}

	while (SQL_SUCCESS == Sql_NextRow(handle)) {
		char* data = nullptr;
		Sql_GetData(handle, 0, &data, nullptr);
		int type = atoi(data);
		Sql_GetData(handle, 1, &data, nullptr);
		int item_id = atoi(data);

		if (type == 0) {
			response["buff_items"].push_back(item_id);
		} else if (type == 3) {
			response["keep_items"].push_back(item_id);
		} else if (type == 4) {
			nlohmann::json row;
			row["item_id"] = item_id;
			Sql_GetData(handle, 2, &data, nullptr);
			row["min_amount"] = atoi(data);
			Sql_GetData(handle, 3, &data, nullptr);
			row["target_amount"] = atoi(data);
			response["buy_items"].push_back(row);
		}
	}
	Sql_FreeResult(handle);

	if (SQL_ERROR == Sql_Query(handle, "SELECT `nameid`, SUM(`amount`) FROM `inventory` WHERE `char_id` = %d AND `amount` > 0 GROUP BY `nameid`", char_id)) {
		Sql_ShowDebug(handle);
	} else {
		while (SQL_SUCCESS == Sql_NextRow(handle)) {
			char* data = nullptr;
			int item_id = 0, amount = 0;
			
			Sql_GetData(handle, 0, &data, nullptr);
			if (data) item_id = atoi(data);
			
			Sql_GetData(handle, 1, &data, nullptr);
			if (data) amount = atoi(data);

			if (item_id > 0) {
				nlohmann::json item_node;
				item_node["id"] = item_id;
				item_node["amount"] = amount;
				response["inventory"].push_back(item_node);
			}
		}
		Sql_FreeResult(handle);
	}

	sl.unlock();
	res.set_content(response.dump(), "application/json");
}

HANDLER_FUNC(autoattack_status)
{
	int account_id = 0, char_id = 0;
	if (!autoattack_require_auth(req, res, account_id, char_id))
		return;

	nlohmann::json response;
	response["has_data"] = false;

	SQLLock sl(MAP_SQL_LOCK);
	sl.lock();
	Sql* handle = sl.getHandle();

	if (req.has_file("cmd")) {
		std::string cmd = req.get_file_value("cmd").content;
		Sql_Query(handle, "UPDATE `aa_runtime_status` SET `command`='%s' WHERE `char_id`=%d", cmd.c_str(), char_id);
	}

	if (SQL_ERROR == Sql_Query(handle,
		"SELECT CAST(CONVERT(`char_name` USING utf8mb4) AS BINARY),`ai_active`,CAST(CONVERT(`state` USING utf8mb4) AS BINARY),CAST(CONVERT(`map` USING utf8mb4) AS BINARY),`x`,`y`,`hp`,`max_hp`,`sp`,`max_sp`,`weight`,`max_weight`,`zeny`,`cash`,`target_id`,CAST(CONVERT(`target_name` USING utf8mb4) AS BINARY),`pickup_item_id`,CAST(CONVERT(`pickup_item_name` USING utf8mb4) AS BINARY),TIMESTAMPDIFF(SECOND,`updated_at`,NOW()), `base_exp`, `max_base_exp`, `job_exp`, `max_job_exp` "
		"FROM `aa_runtime_status` WHERE `account_id` = %d AND `char_id` = %d LIMIT 1",
		account_id, char_id)
	) {
		Sql_FreeResult(handle);
		sl.unlock();
		res.status = 500;
		res.set_content("Database Error", "text/plain");
		return;
	}

	if (SQL_SUCCESS == Sql_NextRow(handle)) {
		char* data = nullptr;
		response["has_data"] = true;

		Sql_GetData(handle, 0, &data, nullptr); response["char_name"] = data ? data : "";
		Sql_GetData(handle, 1, &data, nullptr); response["ai_active"] = data && atoi(data) > 0;
		Sql_GetData(handle, 2, &data, nullptr); response["state"] = data ? data : "";
		Sql_GetData(handle, 3, &data, nullptr); response["map"] = data ? data : "";
		Sql_GetData(handle, 4, &data, nullptr); response["x"] = data ? atoi(data) : 0;
		Sql_GetData(handle, 5, &data, nullptr); response["y"] = data ? atoi(data) : 0;
		Sql_GetData(handle, 6, &data, nullptr); response["hp"] = data ? atoi(data) : 0;
		Sql_GetData(handle, 7, &data, nullptr); response["max_hp"] = data ? atoi(data) : 0;
		Sql_GetData(handle, 8, &data, nullptr); response["sp"] = data ? atoi(data) : 0;
		Sql_GetData(handle, 9, &data, nullptr); response["max_sp"] = data ? atoi(data) : 0;
		Sql_GetData(handle, 10, &data, nullptr); response["weight"] = data ? atoi(data) : 0;
		Sql_GetData(handle, 11, &data, nullptr); response["max_weight"] = data ? atoi(data) : 0;
		Sql_GetData(handle, 12, &data, nullptr); response["zeny"] = data ? atoi(data) : 0;
		Sql_GetData(handle, 13, &data, nullptr); response["cash"] = data ? atoi(data) : 0;
		Sql_GetData(handle, 14, &data, nullptr); response["target_id"] = data ? atoi(data) : 0;
		Sql_GetData(handle, 15, &data, nullptr); response["target_name"] = data ? data : "";
		Sql_GetData(handle, 16, &data, nullptr); response["pickup_item_id"] = data ? atoi(data) : 0;
		Sql_GetData(handle, 17, &data, nullptr); response["pickup_item_name"] = data ? data : "";
		Sql_GetData(handle, 18, &data, nullptr); int age = data ? atoi(data) : 9999; response["age"] = age;
		Sql_GetData(handle, 19, &data, nullptr); response["base_exp"] = data ? atoll(data) : 0;
		Sql_GetData(handle, 20, &data, nullptr); response["max_base_exp"] = data ? atoll(data) : 0;
		Sql_GetData(handle, 21, &data, nullptr); response["job_exp"] = data ? atoll(data) : 0;
		Sql_GetData(handle, 22, &data, nullptr); response["max_job_exp"] = data ? atoll(data) : 0;

		response["online"] = age <= 10;
	}
	Sql_FreeResult(handle);

	int stamina = 0, box_stamina = 0;
	if (SQL_SUCCESS == Sql_Query(handle, "SELECT `stamina_time`, `Boxstamina_time` FROM `stamina` WHERE `account_id` = %d", account_id)) {
		if (SQL_SUCCESS == Sql_NextRow(handle)) {
			char* sdata = nullptr;
			Sql_GetData(handle, 0, &sdata, nullptr); if(sdata) stamina = atoi(sdata);
			Sql_GetData(handle, 1, &sdata, nullptr); if(sdata) box_stamina = atoi(sdata);
		}
		Sql_FreeResult(handle);
	}
	response["stamina"] = stamina + box_stamina;

	sl.unlock();
	res.set_content(response.dump(), "application/json");
}

HANDLER_FUNC(autoattack_config_save)
{
	int account_id = 0, char_id = 0;
	if (!autoattack_require_auth(req, res, account_id, char_id))
		return;

	if (!req.has_file("data")) {
		res.status = HTTP_BAD_REQUEST;
		res.set_content("ไม่มีข้อมูล", "text/plain; charset=utf-8");
		return;
	}

	auto data = nlohmann::json::parse(req.get_file_value("data").content);
	auto buff_ids = autoattack_read_buff_item_ids();

	SQLLock sl(MAP_SQL_LOCK);
	sl.lock();
	Sql* handle = sl.getHandle();

	if (SQL_ERROR == Sql_Query(handle, "START TRANSACTION")
		|| SQL_ERROR == Sql_Query(handle, "DELETE FROM `aa_items` WHERE `char_id` = %d AND `type` IN (0,3,4)", char_id)
	) {
		Sql_ShowDebug(handle);
		Sql_Query(handle, "ROLLBACK");
		sl.unlock();
		res.status = HTTP_BAD_REQUEST;
		res.set_content("บันทึกไม่ได้", "text/plain; charset=utf-8");
		return;
	}

	bool ok = true;
	for (const auto& item : data.value("buff_items", nlohmann::json::array())) {
		int item_id = item.get<int>();
		if (item_id <= 0 || buff_ids.find(item_id) == buff_ids.end())
			continue;

		if (SQL_ERROR == Sql_Query(handle,
			"INSERT INTO `aa_items` (`char_id`,`type`,`item_id`) VALUES (%d,0,%d)",
			char_id, item_id)
		) {
			Sql_ShowDebug(handle);
			ok = false;
			break;
		}
	}

	if (ok) {
		for (const auto& item : data.value("keep_items", nlohmann::json::array())) {
			int item_id = item.get<int>();
			if (item_id <= 0)
				continue;

			if (SQL_ERROR == Sql_Query(handle,
				"INSERT INTO `aa_items` (`char_id`,`type`,`item_id`) VALUES (%d,3,%d)",
				char_id, item_id)
			) {
				Sql_ShowDebug(handle);
				ok = false;
				break;
			}
		}
	}

	if (ok) {
		for (const auto& item : data.value("buy_items", nlohmann::json::array())) {
			int item_id = item.value("item_id", 0);
			int min_amount = item.value("min_amount", 0);
			int target_amount = item.value("target_amount", 0);
			if (item_id <= 0 || min_amount < 0 || target_amount <= min_amount)
				continue;

			if (SQL_ERROR == Sql_Query(handle,
				"INSERT INTO `aa_items` (`char_id`,`type`,`item_id`,`min_hp`,`min_sp`) VALUES (%d,4,%d,%d,%d)",
				char_id, item_id, min_amount, target_amount)
			) {
				Sql_ShowDebug(handle);
				ok = false;
				break;
			}
		}
	}

	if (!ok || SQL_ERROR == Sql_Query(handle, "COMMIT")) {
		Sql_ShowDebug(handle);
		Sql_Query(handle, "ROLLBACK");
		sl.unlock();
		res.status = HTTP_BAD_REQUEST;
		res.set_content("บันทึกไม่ได้", "text/plain; charset=utf-8");
		return;
	}

	sl.unlock();
	res.set_content("{\"ok\":true}", "application/json");
}