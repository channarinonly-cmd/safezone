// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "loginclif.hpp"

#include <stdlib.h>
#include <string.h>

#include <common/malloc.hpp>
#include <common/md5calc.hpp>
#include <common/packets.hpp>
#include <common/random.hpp>
#include <common/showmsg.hpp> //show notice
#include <common/socket.hpp> //wfifo session
#include <common/strlib.hpp> //safeprint
#include <common/timer.hpp> //difftick
#include <common/utils.hpp>

#include "account.hpp"
#include "ipban.hpp" //ipban_check
#include "login.hpp"
#include "loginchrif.hpp"
#include "loginlog.hpp"

#include <ctime>
#include <string>
#include <unordered_map>
#include <config/lionshield.hpp>

// LionShield - vincula o 0x5560 a uma sessao pendente via token one-shot.
namespace {
	struct ShieldPendingPreauth {
		int32 fd;
		uint32_t issued_sec;
	};
	struct PACKET_LABX_PREAUTH_CHALLENGE {
		uint16 packetType;
		uint16 packetLen;
		uint32 server_nonce;
		uint32 preauth_token;
		uint32 padding;
	};
	static constexpr uint16 LIONSHIELD_LOGIN_CHALLENGE_PACKET = 0x5564;
	static constexpr uint16 LIONSHIELD_HWID_PACKET_LEN = 208;
	static constexpr uint32 LIONSHIELD_PREAUTH_TTL_SEC = 15;
	static std::unordered_map<uint32_t, ShieldPendingPreauth> g_shield_preauth_tokens;
	static uint32_t g_shield_preauth_cleanup = 0;

	static void shield_cleanup_preauth_tokens(uint32_t now) {
		if (now < g_shield_preauth_cleanup)
			return;

		for (auto it = g_shield_preauth_tokens.begin(); it != g_shield_preauth_tokens.end(); ) {
			bool expired = now - it->second.issued_sec > 60;
			bool invalid_fd = !session_isValid(it->second.fd) || session[it->second.fd] == nullptr || session[it->second.fd]->session_data == nullptr;
			if (expired || invalid_fd) {
				if (!invalid_fd) {
					auto* sd = (struct login_session_data*)session[it->second.fd]->session_data;
					if (sd != nullptr && sd->shield_preauth_token == it->first)
						sd->shield_preauth_token = 0;
				}
				it = g_shield_preauth_tokens.erase(it);
			} else {
				++it;
			}
		}

		g_shield_preauth_cleanup = now + 120;
	}

	static void shield_unregister_preauth_token(struct login_session_data* sd) {
		if (sd == nullptr || sd->shield_preauth_token == 0)
			return;

		g_shield_preauth_tokens.erase(sd->shield_preauth_token);
		sd->shield_preauth_token = 0;
	}

	static uint32_t shield_issue_preauth_token(int32 fd, struct login_session_data* sd) {
		uint32_t now = (uint32_t)time(nullptr);
		shield_cleanup_preauth_tokens(now);
		shield_unregister_preauth_token(sd);

		uint32_t token = 0;
		do {
			token = lion_secure_rnd32();
		} while (token == 0 || g_shield_preauth_tokens.find(token) != g_shield_preauth_tokens.end());

		g_shield_preauth_tokens[token] = { fd, now };
		sd->shield_preauth_token = token;
		return token;
	}

	static struct login_session_data* shield_consume_preauth_target(uint32_t token, uint32_t source_ip) {
		if (token == 0)
			return nullptr;

		uint32_t now = (uint32_t)time(nullptr);
		shield_cleanup_preauth_tokens(now);

		auto it = g_shield_preauth_tokens.find(token);
		if (it == g_shield_preauth_tokens.end())
			return nullptr;

		int32 target_fd = it->second.fd;
		g_shield_preauth_tokens.erase(it);

		if (!session_isValid(target_fd) || session[target_fd] == nullptr || session[target_fd]->session_data == nullptr)
			return nullptr;

		if ((uint32_t)session[target_fd]->client_addr != source_ip)
			return nullptr;

		auto* target_sd = (struct login_session_data*)session[target_fd]->session_data;
		if (target_sd == nullptr || target_sd->shield_preauth_token != token || !target_sd->shield_preauth_pending)
			return nullptr;

		if (now >= target_sd->shield_preauth_expire) {
			target_sd->shield_preauth_token = 0;
			return nullptr;
		}

		target_sd->shield_preauth_token = 0;
		return target_sd;
	}

} // namespace

static void shield_send_preauth_challenge(int32 fd, struct login_session_data* sd) {
	if (sd == nullptr)
		return;

	uint32 now = (uint32)time(nullptr);
	sd->shield_preauth_nonce = lion_secure_rnd32();
	sd->shield_preauth_expire = now + LIONSHIELD_PREAUTH_TTL_SEC;
	uint32 token = shield_issue_preauth_token(fd, sd);

	WFIFOHEAD(fd, sizeof(PACKET_LABX_PREAUTH_CHALLENGE));
	WFIFOW(fd, 0) = LIONSHIELD_LOGIN_CHALLENGE_PACKET;
	WFIFOW(fd, 2) = sizeof(PACKET_LABX_PREAUTH_CHALLENGE);
	WFIFOL(fd, 4) = sd->shield_preauth_nonce;
	WFIFOL(fd, 8) = token;
	WFIFOL(fd, 12) = 0; // padding
	WFIFOSET(fd, sizeof(PACKET_LABX_PREAUTH_CHALLENGE));
	ShowInfo("[LionShield] Preauth challenge enviado para fd=%d nonce=%08X token=%08X\n", fd, sd->shield_preauth_nonce, token);
}

static bool shield_defer_login_until_preauth(int32 fd, struct login_session_data* sd, const char* ip) {
	if (!login_config.lionshield_enable)
		return false;

	if (sd == nullptr || sd->shield_hwid[0] != '\0')
		return false;

	shield_send_preauth_challenge(fd, sd);
	sd->shield_preauth_pending = true;
	ShowInfo("[LionShield] Preauth challenge enviado para IP: %s nonce=%08X token=%08X\n", ip, sd->shield_preauth_nonce, sd->shield_preauth_token);
	return true;
}

/**
 * Transmit auth result to client.
 * @param fd: client file desciptor link
 * @param result: result to transmit to client, see below
 *  1 : Server closed
 *  2 : Someone has already logged in with this id
 *  8 : already online
 * <result>.B (SC_NOTIFY_BAN)
 */
static void logclif_sent_auth_result(int fd,char result){
	PACKET_SC_NOTIFY_BAN p = {};

	p.packetType = HEADER_SC_NOTIFY_BAN;
	p.result = result;

	socket_send( fd, p );
}

/**
 * Auth successful, inform client and create a temp auth_node.
 * @param sd: player session
 */
static void logclif_auth_ok(struct login_session_data* sd) {
	int fd = sd->fd;
	uint32 ip = session[fd]->client_addr;

	uint8 server_num, n;
	uint32 subnet_char_ip;
	int i;

	if( !global_core->is_running() ){
		// players can only login while running
		logclif_sent_auth_result(fd,1); // server closed
		return;
	}

	if( login_config.group_id_to_connect >= 0 && sd->group_id != login_config.group_id_to_connect ) {
		ShowStatus("Connection refused: the required group id for connection is %d (account: %s, group: %d).\n", login_config.group_id_to_connect, sd->userid, sd->group_id);
		logclif_sent_auth_result(fd,1); // server closed
		return;
	} else if( login_config.min_group_id_to_connect >= 0 && login_config.group_id_to_connect == -1 && sd->group_id < login_config.min_group_id_to_connect ) {
		ShowStatus("Connection refused: the minimum group id required for connection is %d (account: %s, group: %d).\n", login_config.min_group_id_to_connect, sd->userid, sd->group_id);
		logclif_sent_auth_result(fd,1); // server closed
		return;
	}

	server_num = 0;
	for( i = 0; i < ARRAYLENGTH(ch_server); ++i )
		if( session_isActive(ch_server[i].fd) )
			server_num++;

	if( server_num == 0 )
	{// if no char-server, don't send void list of servers, just disconnect the player with proper message
		ShowStatus("Connection refused: there is no char-server online (account: %s).\n", sd->userid);
		logclif_sent_auth_result(fd,1); // server closed
		return;
	}

	{
		struct online_login_data* data = login_get_online_user( sd->account_id );

		if( data )
		{// account is already marked as online!
			if( data->char_server > -1 )
			{// Request char servers to kick this account out. [Skotlex]
				uint8 buf[6];
				ShowNotice("User '%s' is already online - Rejected.\n", sd->userid);
				WBUFW(buf,0) = 0x2734;
				WBUFL(buf,2) = sd->account_id;
				logchrif_sendallwos(-1, buf, 6);
				if( data->waiting_disconnect == INVALID_TIMER )
					data->waiting_disconnect = add_timer(gettick()+AUTH_TIMEOUT, login_waiting_disconnect_timer, sd->account_id, 0);
				logclif_sent_auth_result(fd,8); // 08 = Server still recognizes your last login
				return;
			}
			else
			if( data->char_server == -1 )
			{// client has authed but did not access char-server yet
				// wipe previous session
				login_remove_auth_node(sd->account_id);
				login_remove_online_user(sd->account_id);
				data = NULL;
			}
		}
	}

	// LionShield - preauth check (controlled by config)
	if (login_config.lionshield_enable && sd->shield_hwid[0] == '\0') {
		ShowWarning("[LionShield] Login BLOQUEADO - preauth 0x5560 ausente/incompleto. IP: %s conta: %s\n",
			ip2str(ip, nullptr), sd->userid);
		logclif_sent_auth_result(fd, 3); // Rejected from Server
		return;
	}

	login_log(ip, sd->userid, 100, "login ok");


	ShowStatus("Connection of the account '%s' accepted.\n", sd->userid);

	PACKET_AC_ACCEPT_LOGIN* p = (PACKET_AC_ACCEPT_LOGIN*)packet_buffer;

	p->packetType = HEADER_AC_ACCEPT_LOGIN;
	p->packetLength = sizeof( *p );
	p->login_id1 = sd->login_id1;
	p->AID = sd->account_id;
	p->login_id2 = sd->login_id2;
	// in old version, that was for ip (not more used)
	p->last_ip = 0;
	// in old version, that was for last login time (not more used)
	safestrncpy( p->last_login, "", sizeof( p->last_login ) );
	p->sex = sex_str2num( sd->sex );
#if PACKETVER >= 20170315
	safestrncpy( p->token, sd->web_auth_token, WEB_AUTH_TOKEN_LENGTH ); // web authentication token
#endif

	for( i = 0, n = 0; i < ARRAYLENGTH(ch_server); ++i ) {
		if( !session_isValid(ch_server[i].fd) )
			continue;
		subnet_char_ip = lan_subnetcheck(ip); // Advanced subnet check [LuzZza]

		PACKET_AC_ACCEPT_LOGIN_sub& char_server = p->char_servers[n];

		char_server.ip = htonl( ( subnet_char_ip ) ? subnet_char_ip : ch_server[i].ip );
		char_server.port = ntows( htons( ch_server[i].port ) ); // [!] LE byte order here [!]
		safestrncpy( char_server.name, ch_server[i].name, sizeof( char_server.name ) );
		char_server.users = login_get_usercount( ch_server[i].users );
		char_server.type = ch_server[i].type;
		char_server.new_ = ch_server[i].new_;
#if PACKETVER >= 20170315
		memset( &char_server.unknown, 0, sizeof( char_server.unknown ) );
#endif

#ifdef DEBUG
		ShowDebug(
			"Sending the client (%d %d.%d.%d.%d) to char-server %s with ip %d.%d.%d.%d and port "
			"%hu\n",
			sd->account_id, CONVIP(ip), ch_server[i].name,
			CONVIP((subnet_char_ip) ? subnet_char_ip : ch_server[i].ip), ch_server[i].port);
#endif

		n++;
		p->packetLength += sizeof( char_server );
	}

	socket_send( fd, p );

	// create temporary auth entry
	login_add_auth_node( sd, ip );

	// mark client as 'online'
	struct online_login_data* data = login_add_online_user(-1, sd->account_id);
	// schedule deletion of this node
	data->waiting_disconnect = add_timer(gettick()+AUTH_TIMEOUT, login_waiting_disconnect_timer, sd->account_id, 0);
}

static void logclif_auth_failed( int fd, int result, const char* unblock_time = "" ){
	PACKET_AC_REFUSE_LOGIN p = {};

	p.packetType = HEADER_AC_REFUSE_LOGIN;
	p.error = result;
	safestrncpy( p.unblock_time, "", sizeof( p.unblock_time ) );

	socket_send( fd, p );
}

/**
 * Inform client that auth has failed.
 * @param sd: player session
 * @param result: nb (msg define in conf)
    0 = Unregistered ID
    1 = Incorrect Password
    2 = This ID is expired
    3 = Rejected from Server
    4 = You have been blocked by the GM Team
    5 = Your Game's EXE file is not the latest version
    6 = You are prohibited to log in until %s
    7 = Server is jammed due to over populated
    8 = No more accounts may be connected from this company
    9 = MSI_REFUSE_BAN_BY_DBA
    10 = MSI_REFUSE_EMAIL_NOT_CONFIRMED
    11 = MSI_REFUSE_BAN_BY_GM
    12 = MSI_REFUSE_TEMP_BAN_FOR_DBWORK
    13 = MSI_REFUSE_SELF_LOCK
    14 = MSI_REFUSE_NOT_PERMITTED_GROUP
    15 = MSI_REFUSE_NOT_PERMITTED_GROUP
    99 = This ID has been totally erased
    100 = Login information remains at %s
    101 = Account has been locked for a hacking investigation. Please contact the GM Team for more information
    102 = This account has been temporarily prohibited from login due to a bug-related investigation
    103 = This character is being deleted. Login is temporarily unavailable for the time being
    104 = This character is being deleted. Login is temporarily unavailable for the time being
     default = Unknown Error.
 */
static void logclif_auth_failed(struct login_session_data* sd, int result) {
	int fd = sd->fd;
	uint32 ip = session[fd]->client_addr;

	if (login_config.log_login)
	{
		if(result >= 0 && result <= 15)
		    login_log(ip, sd->userid, result, msg_txt(result));
		else if(result >= 99 && result <= 104)
		    login_log(ip, sd->userid, result, msg_txt(result-83)); //-83 offset
		else
		    login_log(ip, sd->userid, result, msg_txt(22)); //unknow error


	}

	if( (result == 0 || result == 1) && login_config.dynamic_pass_failure_ban )
		ipban_log(ip); // log failed password attempt

	// 6 = You are prohibited to log in until %s
	if( result == 6 ){
		char unblock_time[20];
		struct mmo_account acc;
		AccountDB* accounts = login_get_accounts_db();
		time_t unban_time = ( accounts->load_str( accounts, &acc, sd->userid ) ) ? acc.unban_time : 0;
		timestamp2string( unblock_time, sizeof( unblock_time ), unban_time, login_config.date_format );

		logclif_auth_failed( fd, result, unblock_time );
	}else{
		logclif_auth_failed( fd, result );
	}
}

/**
 * Received a keepalive packet to maintain connection.
 * 0x200 <account.userid>.24B.
 * @param fd: fd to parse from (client fd)
 * @return 0 not enough info transmitted, 1 success
 */
static bool logclif_parse_keepalive( int fd, struct login_session_data& ){
	// Do nothing
	return true;
}

/**
 * Received a keepalive packet to maintain connection.
 * S 0204 <md5 hash>.16B (kRO 2004-05-31aSakexe langtype 0 and 6)
 * @param fd: fd to parse from (client fd)
 * @return 0 not enough info transmitted, 1 success
 */
static bool logclif_parse_updclhash( int fd, struct login_session_data& sd ){
	PACKET_CA_EXE_HASHCHECK* p = (PACKET_CA_EXE_HASHCHECK*)RFIFOP( fd, 0 );

	sd.has_client_hash = 1;
	memcpy( sd.client_hash, p->hash, sizeof( sd.client_hash ) );

	return true;
}

template <typename P>
static bool logclif_parse_reqauth_raw( int fd, login_session_data& sd ){
	P* p = (P*)RFIFOP( fd, 0 );

	char ip[16];
	uint32 ipl = session[fd]->client_addr;
	ip2str( ipl, ip );

	safestrncpy( sd.userid, p->username, sizeof( sd.userid ) );
	sd.clienttype = p->clienttype;

	ShowStatus( "Request for connection of %s (ip: %s)\n", sd.userid, ip );
	safestrncpy( sd.passwd, p->password, PASSWD_LENGTH );

	if( login_config.use_md5_passwds ){
		MD5_String( sd.passwd, sd.passwd );
	}

	sd.passwdenc = 0;
	if (shield_defer_login_until_preauth(fd, &sd, ip))
		return true;

	int result = login_mmo_auth( &sd, false );

	if( result == -1 ){
		logclif_auth_ok( &sd );
	}else{
		logclif_auth_failed( &sd, result );
	}

	return true;
}

template <typename P>
static bool logclif_parse_reqauth_md5( int fd, login_session_data& sd ){
	P* p = (P*)RFIFOP( fd, 0 );

	char ip[16];
	uint32 ipl = session[fd]->client_addr;
	ip2str( ipl, ip );

	safestrncpy( sd.userid, p->username, sizeof( sd.userid ) );
	sd.clienttype = p->clienttype;

	ShowStatus( "Request for connection (passwdenc mode) of %s (ip: %s)\n", sd.userid, ip );
	bin2hex( sd.passwd, p->passwordMD5, sizeof( p->passwordMD5 ) ); // raw binary data here!

	sd.passwdenc = PASSWORDENC;

	if( login_config.use_md5_passwds ){
		logclif_auth_failed( &sd, 3 ); // send "rejected from server"
		return false;
	}

	if (shield_defer_login_until_preauth(fd, &sd, ip))
		return true;

	int result = login_mmo_auth( &sd, false );

	if( result == -1 ){
		logclif_auth_ok( &sd );
	}else{
		logclif_auth_failed( &sd, result );
	}

	return true;
}

template <typename P>
static bool logclif_parse_reqauth_sso( int fd, login_session_data& sd ){
	P* p = (P*)RFIFOP( fd, 0 );

	char ip[16];
	uint32 ipl = session[fd]->client_addr;
	ip2str( ipl, ip );

	size_t token_length = p->packetLength - sizeof( *p );

	safestrncpy( sd.userid, p->username, sizeof( sd.userid ) );
	sd.clienttype = p->clienttype;

	ShowStatus( "Request for connection (SSO mode) of %s (ip: %s)\n", sd.userid, ip );
	// Shinryo: For the time being, just use token as password.
	safestrncpy( sd.passwd, p->token, token_length + 1 );

	if( login_config.use_md5_passwds ){
		MD5_String( sd.passwd, sd.passwd );
	}

	sd.passwdenc = 0;
	if (shield_defer_login_until_preauth(fd, &sd, ip))
		return true;

	int result = login_mmo_auth( &sd, false );

	if( result == -1 ){
		logclif_auth_ok( &sd );
	}else{
		logclif_auth_failed( &sd, result );
	}

	return true;
}

static void logclif_reqkey_result( int fd, struct login_session_data& sd ){
	PACKET_AC_ACK_HASH* p = (PACKET_AC_ACK_HASH*)packet_buffer;

	p->packetType = HEADER_AC_ACK_HASH;
	p->packetLength = sizeof( *p ) + sd.md5keylen;
	strncpy( p->salt, sd.md5key, sd.md5keylen );

	socket_send( fd, p );
}

/**
 * Client requests an md5key for his session: keys will be generated and sent back.
 * @param fd: file descriptor to parse from (client)
 * @param sd: client session
 * @return 1 success
 */
static bool logclif_parse_reqkey( int fd, struct login_session_data& sd ){
	PACKET_CA_REQ_HASH* p_in = (PACKET_CA_REQ_HASH*)RFIFOP( fd, 0 );

	sd.md5keylen = sizeof( sd.md5key );
	MD5_Salt( sd.md5keylen, sd.md5key );

	logclif_reqkey_result( fd, sd );

	return 1;
}

/**
 * Char-server request to connect to the login-server.
 * This is needed to exchange packets.
 * @param fd: file descriptor to parse from (client)
 * @param sd: client session
 * @param ip: ipv4 address (client)
 * @return 0 packet received too shirt, 1 success
 */
static int logclif_parse_reqcharconnec(int fd, struct login_session_data *sd, char* ip){
	if (RFIFOREST(fd) < 86)
		return 0;
	else {
		int result;
		char server_name[20];
		char message[256];
		uint32 server_ip;
		uint16 server_port;
		uint16 type;
		uint16 new_;

		safestrncpy(sd->userid, RFIFOCP(fd,2), NAME_LENGTH);
		safestrncpy(sd->passwd, RFIFOCP(fd,26), NAME_LENGTH);
		if( login_config.use_md5_passwds )
			MD5_String(sd->passwd, sd->passwd);
		sd->passwdenc = 0;
		server_ip = ntohl(RFIFOL(fd,54));
		server_port = ntohs(RFIFOW(fd,58));
		safestrncpy(server_name, RFIFOCP(fd,60), 20);
		type = RFIFOW(fd,82);
		new_ = RFIFOW(fd,84);
		RFIFOSKIP(fd,86);

		ShowInfo("Connection request of the char-server '%s' @ %u.%u.%u.%u:%u (account: '%s', ip: '%s')\n", server_name, CONVIP(server_ip), server_port, sd->userid, ip);
		sprintf(message, "charserver - %s@%u.%u.%u.%u:%u", server_name, CONVIP(server_ip), server_port);
		login_log(session[fd]->client_addr, sd->userid, 100, message);


		result = login_mmo_auth(sd, true);
		if( global_core->is_running() &&
			result == -1 &&
			sd->sex == 'S' &&
			sd->account_id < ARRAYLENGTH(ch_server) &&
			!session_isValid(ch_server[sd->account_id].fd) )
		{
			ShowStatus("Connection of the char-server '%s' accepted.\n", server_name);
			safestrncpy(ch_server[sd->account_id].name, server_name, sizeof(ch_server[sd->account_id].name));
			ch_server[sd->account_id].fd = fd;
			ch_server[sd->account_id].ip = server_ip;
			ch_server[sd->account_id].port = server_port;
			ch_server[sd->account_id].users = 0;
			ch_server[sd->account_id].type = type;
			ch_server[sd->account_id].new_ = new_;

			session[fd]->func_parse = logchrif_parse;
			session[fd]->flag.server = 1;
			realloc_fifo(fd, FIFOSIZE_SERVERLINK, FIFOSIZE_SERVERLINK);

			// send connection success
			WFIFOHEAD(fd,3);
			WFIFOW(fd,0) = 0x2711;
			WFIFOB(fd,2) = 0;
			WFIFOSET(fd,3);
		}
		else
		{
			ShowNotice("Connection of the char-server '%s' REFUSED.\n", server_name);
			WFIFOHEAD(fd,3);
			WFIFOW(fd,0) = 0x2711;
			WFIFOB(fd,2) = 3;
			WFIFOSET(fd,3);
		}
	}
	return 1;
}

static void logclif_otp_result( int fd ){
	PACKET_TC_RESULT p = {};

	p.packetType = HEADER_TC_RESULT;
	p.packetLength = sizeof( p );
	p.type = 0; // normal login
	safestrncpy( p.unknown1, "S1000", sizeof( p.unknown1 ) );
	safestrncpy( p.unknown2, "token", sizeof( p.unknown2 ) );

	socket_send( fd, p );
}

static bool logclif_parse_otp_login( int fd, struct login_session_data& ){
	PACKET_CT_AUTH* p = (PACKET_CT_AUTH*)RFIFOP( fd, 0 );

	logclif_otp_result( fd );

	return 1;
}

class LoginPacketDatabase : public PacketDatabase<login_session_data>{
public:
	LoginPacketDatabase(){
		this->add( HEADER_CA_CONNECT_INFO_CHANGED, true, sizeof( PACKET_CA_CONNECT_INFO_CHANGED ), logclif_parse_keepalive );
		this->add( HEADER_CA_EXE_HASHCHECK, true, sizeof( PACKET_CA_EXE_HASHCHECK ), logclif_parse_updclhash );
		this->add( HEADER_CA_LOGIN, true, sizeof( PACKET_CA_LOGIN ), logclif_parse_reqauth_raw<PACKET_CA_LOGIN> );
		this->add( HEADER_CA_LOGIN_PCBANG, true, sizeof( PACKET_CA_LOGIN_PCBANG ), logclif_parse_reqauth_raw<PACKET_CA_LOGIN_PCBANG> );
		this->add( HEADER_CA_LOGIN_CHANNEL, true, sizeof( PACKET_CA_LOGIN_CHANNEL ), logclif_parse_reqauth_raw<PACKET_CA_LOGIN_CHANNEL> );
		this->add( HEADER_CA_LOGIN2, true, sizeof( PACKET_CA_LOGIN2 ), logclif_parse_reqauth_md5<PACKET_CA_LOGIN2> );
		this->add( HEADER_CA_LOGIN3, true, sizeof( PACKET_CA_LOGIN3 ), logclif_parse_reqauth_md5<PACKET_CA_LOGIN3> );
		this->add( HEADER_CA_LOGIN4, true, sizeof( PACKET_CA_LOGIN4 ), logclif_parse_reqauth_md5<PACKET_CA_LOGIN4> );
		this->add( HEADER_CA_SSO_LOGIN_REQ, false, sizeof( PACKET_CA_SSO_LOGIN_REQ ), logclif_parse_reqauth_sso<PACKET_CA_SSO_LOGIN_REQ> );
		this->add( HEADER_CA_REQ_HASH, true, sizeof( PACKET_CA_REQ_HASH ), logclif_parse_reqkey );
		this->add( HEADER_CT_AUTH, true, sizeof( PACKET_CT_AUTH ), logclif_parse_otp_login );
	}
} login_packet_db;

/**
 * Entry point from client to log-server.
 * Function that checks incoming command, then splits it to the correct handler.
 * @param fd: file descriptor to parse, (link to client)
 * @return 0=invalid session,marked for disconnection,unknow packet, banned..; 1=success
 */
int logclif_parse(int fd) {
	struct login_session_data* sd = (struct login_session_data*)session[fd]->session_data;

	char ip[16];
	uint32 ipl = session[fd]->client_addr;
	ip2str(ipl, ip);

	if( session[fd]->flag.eof )
	{
		ShowInfo("Closed connection from '" CL_WHITE "%s" CL_RESET "'.\n", ip);
		do_close(fd);
		return 0;
	}

	if( sd == nullptr ){
		// Perform ip-ban check
		if( login_config.ipban && ipban_check(ipl) )
		{
			ShowStatus("Connection refused: IP isn't authorised (deny/allow, ip: %s).\n", ip);
			login_log(ipl, "unknown", -3, "ip banned");


			logclif_auth_failed( fd, 3 ); // 3 = Rejected from Server

			set_eof(fd);
			return 0;
		}
		// create a session for this new connection
		CREATE(session[fd]->session_data, struct login_session_data, 1);
		sd = (struct login_session_data*)session[fd]->session_data;
		sd->fd = fd;
	}

	while( RFIFOREST(fd) >= 2 )
	{
		uint16 command = RFIFOW(fd,0);


		switch( command ){
			// Connection request of a char-server
			case 0x2710: logclif_parse_reqcharconnec(fd,sd, ip); return 0; // processing will continue elsewhere
			// LionShield - receive HWID + HMAC (packet 0x5560)
			case 0x5560: {
				if (RFIFOREST(fd) < LIONSHIELD_HWID_PACKET_LEN)
					return 0;
				// Per-IP rate limiting: max 5 auth attempts per 60s
				{
					struct RLEntry { uint32_t count; uint32_t first_sec; };
					static std::unordered_map<uint32_t, RLEntry> s_5560_rl;
					static uint32_t s_next_cleanup = 0;
					uint32_t now_sec = (uint32_t)time(nullptr);
					uint32_t client_ip = (uint32_t)session[fd]->client_addr;
					if (now_sec >= s_next_cleanup) {
						for (auto it = s_5560_rl.begin(); it != s_5560_rl.end(); )
							it = (now_sec - it->second.first_sec > 60) ? s_5560_rl.erase(it) : ++it;
						s_next_cleanup = now_sec + 120;
					}
					auto& rl = s_5560_rl[client_ip];
					if (now_sec - rl.first_sec > 60) { rl.count = 0; rl.first_sec = now_sec; }
					if (++rl.count > 5) {
						ShowWarning("[LionShield] 0x5560 rate limit exceeded IP: %s -- disconnecting\n", ip);
						set_eof(fd);
						return 0;
					}
				}
				char hwid_tmp[65] = {};
				uint32_t client_nonce = RFIFOL(fd, 68);
				uint32_t server_nonce = RFIFOL(fd, 72);
				uint32_t preauth_token = RFIFOL(fd, 76);
				char dll_hash_tmp[65] = {};
				char hmac_tmp[65] = {};
				memcpy(hwid_tmp, RFIFOP(fd, 4),  64);
				memcpy(dll_hash_tmp, RFIFOP(fd, 80), 64);
				memcpy(hmac_tmp, RFIFOP(fd, 144), 64);
				auto is_hex64 = [](const char* p) {
					for (int i = 0; i < 64; i++) {
						char c = p[i];
						if (!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'))) return false;
					}
					return true;
				};
				if (!is_hex64(hwid_tmp) || !is_hex64(dll_hash_tmp) || !is_hex64(hmac_tmp)) {
					ShowWarning("[LionShield] Invalid HWID packet from IP: %s\n", ip);
					RFIFOSKIP(fd, LIONSHIELD_HWID_PACKET_LEN); break;
				}
				auto* target_sd = shield_consume_preauth_target(preauth_token, (uint32_t)session[fd]->client_addr);
				if (target_sd == nullptr) {
					ShowWarning("[LionShield] Invalid preauth token from IP: %s token=%08X\n", ip, preauth_token);
					RFIFOSKIP(fd, LIONSHIELD_HWID_PACKET_LEN); break;
				}
				if (target_sd->shield_hwid[0] != '\0') {
					ShowWarning("[LionShield] Duplicate 0x5560 from IP: %s token=%08X\n", ip, preauth_token);
					RFIFOSKIP(fd, LIONSHIELD_HWID_PACKET_LEN); break;
				}
				uint32 now_sec = (uint32)time(nullptr);
				if (target_sd->shield_preauth_nonce == 0 || target_sd->shield_preauth_nonce != server_nonce || now_sec >= target_sd->shield_preauth_expire) {
					ShowWarning("[LionShield] Invalid or expired preauth nonce from IP: %s\n", ip);
					RFIFOSKIP(fd, LIONSHIELD_HWID_PACKET_LEN); break;
				}
				std::string hwid_str(hwid_tmp);
				for (auto& c : hwid_str) c = toupper(c);
				std::string dll_hash_str(dll_hash_tmp);
				for (auto& c : dll_hash_str) c = toupper(c);
				char client_nonce_hex[9] = {};
				char server_nonce_hex[9] = {};
				char token_hex[9] = {};
				snprintf(client_nonce_hex, sizeof(client_nonce_hex), "%08X", client_nonce);
				snprintf(server_nonce_hex, sizeof(server_nonce_hex), "%08X", server_nonce);
				snprintf(token_hex, sizeof(token_hex), "%08X", preauth_token);
				std::string expected = lion_sha256(hwid_str + client_nonce_hex + server_nonce_hex + token_hex + dll_hash_str + "P0T2" + SHIELD_SECRET_KEY);
				std::string received(hmac_tmp);
				for (auto& c : received) c = tolower(c);
				if (received != expected) {
					std::string debug_str = hwid_str + client_nonce_hex + server_nonce_hex + token_hex + dll_hash_str + "P0T2" + SHIELD_SECRET_KEY;
					ShowWarning("[LionShield] HMAC mismatch. IP: %s\n", ip);
					ShowWarning("  Expected: %s\n", expected.c_str());
					ShowWarning("  Received: %s\n", received.c_str());
					ShowWarning("  Debug String: %s\n", debug_str.c_str());
					RFIFOSKIP(fd, LIONSHIELD_HWID_PACKET_LEN); break;
				}
				if (!lion_is_allowed_dll_hash(dll_hash_str)) {
					ShowWarning("[LionShield] DLL hash not allowed from IP: %s hash=%.64s\n", ip, dll_hash_tmp);
					RFIFOSKIP(fd, LIONSHIELD_HWID_PACKET_LEN); break;
				}
				memset(target_sd->shield_hwid, 0, sizeof(target_sd->shield_hwid));
				memcpy(target_sd->shield_hwid, hwid_str.c_str(), 64);
				target_sd->shield_verified = false;
				target_sd->shield_preauth_nonce = 0;
				target_sd->shield_preauth_token = 0;
				target_sd->shield_preauth_expire = 0;
				ShowInfo("[LionShield] HWID preauth accepted: %.64s | IP: %s token=%08X\n", target_sd->shield_hwid, ip, preauth_token);
				RFIFOSKIP(fd, LIONSHIELD_HWID_PACKET_LEN);
				if (target_sd->shield_preauth_pending) {
					target_sd->shield_preauth_pending = false;
					int32 result = login_mmo_auth(target_sd, false);
					if (result == -1) {
						uint64_t uid = lion_hwid_to_unique_id(target_sd->shield_hwid);
						if (LIONSHIELD_MAX_CLIENTS_PER_HWID > 0) {
							int active_clients = login_count_online_shield(uid, target_sd->account_id);
							if (active_clients >= LIONSHIELD_MAX_CLIENTS_PER_HWID) {
								ShowWarning("[LionShield] HWID client limit exceeded. AID:%u HWID:%.64s active:%d limit:%d\n",
									target_sd->account_id, target_sd->shield_hwid, active_clients, LIONSHIELD_MAX_CLIENTS_PER_HWID);
								logclif_auth_failed(target_sd, 3);
								break;
							}
						}
						account_db_shield_save(login_get_accounts_db(), target_sd->account_id, target_sd->shield_hwid, uid);
						logclif_auth_ok(target_sd);
					} else {
						logclif_auth_failed(target_sd, result);
					}
				}
				break;
			}
			default:
				if( !login_packet_db.handle( fd, *sd ) ){
					return 0;
				}
				break;
		}
	}

	return 0;
}

/// Constructor destructor

/**
 * Initialize the module.
 * Launched at login-serv start, create db or other long scope variable here.
 */
void do_init_loginclif(void){
	return;
}

/**
 * loginclif destructor
 *  dealloc..., function called at exit of the login-serv
 */
void do_final_loginclif(void){
	return;
}
