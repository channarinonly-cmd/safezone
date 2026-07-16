// LionShield - Centralized Configuration
// Edit ONLY this file to change the secret key.
// SHIELD_SECRET_KEY must be identical to the key patched in LionShield.dll.
// =============================================================================
#ifndef CONFIG_LIONSHIELD_HPP
#define CONFIG_LIONSHIELD_HPP

#define LIONSHIELD_ENABLED 1
#define SHIELD_SECRET_KEY "LABX_SHIELD_SECRET_KEY_PLACEHOLDER_SIGNATURE_KEY_GOES_HERE_12345"

// SHA-256 of the patched DLL. Leave empty "" to skip hash check.
#define SHIELD_ALLOWED_DLL_HASH "5F151E454F7D53FB1F1616C55E2E86103005691CCDE84DD7550C738CD7F85D6F"

// Max clients per HWID (0 = disabled)
#define LIONSHIELD_MAX_CLIENTS_PER_HWID 2

// LionShield - SHA-256
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>

static inline void lion_sha256_raw(const uint8_t* data, size_t len, uint8_t out[32]) {
	uint32_t k[64] = {
		0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
		0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
		0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
		0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
		0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
		0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
		0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
		0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
	};
	uint32_t h[8] = {
		0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
		0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
	};
	auto RR = [](uint32_t x,int n){ return (x>>n)|(x<<(32-n)); };
	size_t total = ((len+8)/64+1)*64;
	uint8_t* buf = new uint8_t[total]();
	memcpy(buf, data, len);
	buf[len] = 0x80;
	uint64_t bits = (uint64_t)len*8;
	for(int i=0;i<8;i++) buf[total-1-i]=(uint8_t)(bits>>(i*8));
	for(size_t i=0;i<total;i+=64) {
		uint32_t w[64];
		for(int j=0;j<16;j++) w[j]=((uint32_t)buf[i+j*4]<<24)|((uint32_t)buf[i+j*4+1]<<16)|((uint32_t)buf[i+j*4+2]<<8)|buf[i+j*4+3];
		for(int j=16;j<64;j++) { uint32_t s0=RR(w[j-15],7)^RR(w[j-15],18)^(w[j-15]>>3); uint32_t s1=RR(w[j-2],17)^RR(w[j-2],19)^(w[j-2]>>10); w[j]=w[j-16]+s0+w[j-7]+s1; }
		uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
		for(int j=0;j<64;j++) {
			uint32_t S1=RR(e,6)^RR(e,11)^RR(e,25), ch=(e&f)^(~e&g), temp1=hh+S1+ch+k[j]+w[j];
			uint32_t S0=RR(a,2)^RR(a,13)^RR(a,22), maj=(a&b)^(a&c)^(b&c), temp2=S0+maj;
			hh=g; g=f; f=e; e=d+temp1; d=c; c=b; b=a; a=temp1+temp2;
		}
		h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
	}
	delete[] buf;
	for(int i=0;i<8;i++) { out[i*4]=(h[i]>>24)&0xff; out[i*4+1]=(h[i]>>16)&0xff; out[i*4+2]=(h[i]>>8)&0xff; out[i*4+3]=h[i]&0xff; }
}

static inline std::string lion_sha256(const std::string& input) {
	uint8_t hash[32];
	lion_sha256_raw(reinterpret_cast<const uint8_t*>(input.c_str()), input.size(), hash);
	std::string result(64, '0');
	for (int i = 0; i < 32; i++) {
		char buf[3];
		snprintf(buf, sizeof(buf), "%02x", hash[i]);
		result[i*2]   = buf[0];
		result[i*2+1] = buf[1];
	}
	return result;
}

static inline bool lion_is_allowed_dll_hash(const std::string& dll_hash) {
	if (SHIELD_ALLOWED_DLL_HASH[0] == '\0')
		return true;

	std::string hash_upper = dll_hash;
	for (char& c : hash_upper) {
		if (c >= 'a' && c <= 'f')
			c = c - 'a' + 'A';
	}

	return hash_upper == SHIELD_ALLOWED_DLL_HASH;
}

static inline uint64_t lion_hwid_to_unique_id(const char* hwid) {
	if (!hwid || hwid[0] == '\0') return 0;
	uint64_t hash = 1469598103934665603ULL;
	for (int i = 0; i < 64 && hwid[i]; i++) {
		char c = hwid[i];
		if (!((c>='0'&&c<='9')||(c>='A'&&c<='F')||(c>='a'&&c<='f'))) return 0;
		hash ^= (unsigned char)c;
		hash *= 1099511628211ULL;
	}
	hash &= 0x7FFFFFFFFFFFFFFFULL;
	return hash ? hash : 1ULL;
}

// LionShield - portable non-zero 32-bit token generator.
#include <ctime>
static inline uint32_t lion_secure_rnd32(void) {
	static uint32_t s_lcg = 0xC0FFEE37u;
	s_lcg = s_lcg * 1664525u + 1013904223u; // Numerical Recipes LCG
	uint32_t t = (uint32_t)(uintptr_t)time(nullptr);
	uint32_t v = (t * 2654435761u) ^ s_lcg ^ (t >> 17);
	return v ? v : 1u;
}

#endif // CONFIG_LIONSHIELD_HPP
