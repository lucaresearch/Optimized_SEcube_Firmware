/**
 *  \file se3_security_core.c
 *  \author Nicola Ferri
 *  \co-author Filippo Cottone, Pietro Scandale, Francesco Vaiana, Luca Di Grazia
 *  \brief Security core
 */

#include "se3_security_core.h"
#include "se3_flash.h"
#include "se3_algo_Aes.h"
#include "se3_algo_sha256.h"
#include "se3_algo_HmacSha256.h"
#include "se3_algo_AesHmacSha256s.h"
#include "se3_algo_aes256hmacsha256.h"

/* Cryptographic algorithms handlers and display info for the security core ONLY. */
se3_algo_descriptor algo_table[SE3_ALGO_MAX] = {
	{
		se3_algo_Aes_init,
		se3_algo_Aes_update,
		sizeof(B5_tAesCtx),
		"Aes",
		SE3_CRYPTO_TYPE_BLOCKCIPHER,
		B5_AES_BLK_SIZE,
		B5_AES_256 },
	{
		se3_algo_Sha256_init,
		se3_algo_Sha256_update,
		sizeof(B5_tSha256Ctx),
		"Sha256",
		SE3_CRYPTO_TYPE_DIGEST,
		B5_SHA256_DIGEST_SIZE,
		0 },
	{
		se3_algo_HmacSha256_init,
		se3_algo_HmacSha256_update,
		sizeof(B5_tHmacSha256Ctx),
		"HmacSha256",
		SE3_CRYPTO_TYPE_DIGEST,
		B5_SHA256_DIGEST_SIZE,
		B5_AES_256 },
	{
		se3_algo_AesHmacSha256s_init,
		se3_algo_AesHmacSha256s_update,
		sizeof(B5_tAesCtx) + sizeof(B5_tHmacSha256Ctx) + 2 * B5_AES_256 + sizeof(uint16_t) + 3 * sizeof(uint8_t),
		"AesHmacSha256s",
		SE3_CRYPTO_TYPE_BLOCKCIPHER_AUTH,
		B5_AES_BLK_SIZE,
		B5_AES_256 },
	{
		se3_algo_aes256hmacsha256_init,
		se3_algo_aes256hmacsha256_update,
		sizeof(B5_tAesCtx) + sizeof(B5_tHmacSha256Ctx),
		"AES256HMACSHA256",
		SE3_CRYPTO_TYPE_BLOCKCIPHER_AUTH,
		B5_AES_BLK_SIZE,
		B5_AES_256 },
	{ NULL, NULL, 0, "", 0, 0, 0 },
	{ NULL, NULL, 0, "", 0, 0, 0 },
	{ NULL, NULL, 0, "", 0, 0, 0 }
};

union {
    B5_tSha256Ctx sha;
    B5_tAesCtx aes;
} ctx;

void se3_security_core_init(){
    memset(&ctx, 0, sizeof(ctx));
    memset((void*)&se3_security_info, 0, sizeof(SE3_SECURITY_INFO));
}

static bool record_find(uint16_t record_type, se3_flash_it* it)
{
    uint16_t it_record_type = 0;
    while (se3_flash_it_next(it)) {
        if (it->type == SE3_FLASH_TYPE_RECORD) {
            SE3_GET16(it->addr, SE3_RECORD_OFFSET_TYPE, it_record_type);
            if (it_record_type == record_type) {
                return true;
            }
        }
    }
    return false;
}

bool record_set(uint16_t type, const uint8_t* data)
{
    se3_flash_it it;
    bool found = false;
    se3_flash_it it2;
    uint8_t tmp[2];
    if (type >= SE3_RECORD_MAX) {
        return false;
    }
    se3_flash_it_init(&it);
    if (record_find(type, &it)) {
        found = true;
    }

    // allocate new flash block
    memcpy(&it2, &it, sizeof(se3_flash_it));
    if (!se3_flash_it_new(&it2, SE3_FLASH_TYPE_RECORD, SE3_RECORD_SIZE_TYPE + SE3_RECORD_SIZE)) {
        return false;
    }
    // write record type and data
    if (!se3_flash_it_write(&it2, SE3_RECORD_OFFSET_DATA, data, SE3_RECORD_SIZE)) {
        return false;
    }
    SE3_SET16(tmp, 0, type);
    if (!se3_flash_it_write(&it2, SE3_RECORD_OFFSET_TYPE, tmp, SE3_RECORD_SIZE_TYPE)) {
        return false;
    }

    if (found) {
        // delete previously found flash block
        if (!se3_flash_it_delete(&it)) {
            return false;
        }
    }

    return true;
}

bool record_get(uint16_t type, uint8_t* data)
{
    se3_flash_it it;
    if (type >= SE3_RECORD_MAX) {
        return false;
    }
    se3_flash_it_init(&it);
    if (!record_find(type, &it)) {
        return false;
    }
    memcpy(data, it.addr + SE3_RECORD_OFFSET_DATA, SE3_RECORD_SIZE);
    return true;
}

/** \brief initialize a crypto context
 *
 *  crypto_init : (algo:ui16, mode:ui16, key_id:ui32) => (sid:ui32)
 */
uint16_t crypto_init(uint16_t req_size, const uint8_t* req, uint16_t* resp_size, uint8_t* resp)
{
    struct {
        uint16_t algo;
        uint16_t mode;
        uint32_t key_id;
    } req_params;
    struct {
        uint32_t sid;
    } resp_params;

    se3_flash_key key;
    se3_flash_it it = { .addr = NULL };
    se3_crypto_init_handler handler = NULL;
    uint32_t status;
    int sid;
    uint8_t* ctx;

    if (req_size != SE3_CMD1_CRYPTO_INIT_REQ_SIZE) {
        SE3_TRACE(("[crypto_init] req size mismatch\n"));
        return SE3_ERR_PARAMS;
    }

//    if (!se3_security_info.login.y) {
//        SE3_TRACE(("[crypto_init] not logged in\n"));
//        return SE3_ERR_ACCESS;
//    }

    SE3_GET16(req, SE3_CMD1_CRYPTO_INIT_REQ_OFF_ALGO, req_params.algo);
    SE3_GET16(req, SE3_CMD1_CRYPTO_INIT_REQ_OFF_MODE, req_params.mode);
    SE3_GET32(req, SE3_CMD1_CRYPTO_INIT_REQ_OFF_KEY_ID, req_params.key_id);

    if (req_params.algo < SE3_ALGO_MAX) {
        handler = algo_table[req_params.algo].init;
    }
    if (handler == NULL) {
        SE3_TRACE(("[crypto_init] algo not found\n"));
        return SE3_ERR_PARAMS;
    }

    // use request buffer to temporarily store key data
    // !! modifying request buffer
    key.data = (uint8_t*)req + 16;
    key.name = NULL;
    key.id = req_params.key_id;

    if (key.id == SE3_KEY_INVALID) {
        memset(key.data, 0, SE3_KEY_DATA_MAX);
    }
    else {
        se3_flash_it_init(&it);
        if (!se3_key_find(key.id, &it)) {
            it.addr = NULL;
        }
        if (NULL == it.addr) {
            SE3_TRACE(("[crypto_init] key not found\n"));
            return SE3_ERR_RESOURCE;
        }
        se3_key_read(&it, &key);

		if (key.validity < se3_time_get() || !(get_now_initialized())) {
			SE3_TRACE(("[crypto_init] key expired\n"));
			return SE3_ERR_EXPIRED;
		}
    }

    resp_params.sid = SE3_SESSION_INVALID;
    sid = se3_mem_alloc(&(se3_security_info.sessions), algo_table[req_params.algo].size);
    if (sid >= 0) {
        resp_params.sid = (uint32_t)sid;
    }

    if (resp_params.sid == SE3_SESSION_INVALID) {
        SE3_TRACE(("[crypto_init] cannot allocate session\n"));
        return SE3_ERR_MEMORY;
    }

    ctx = se3_mem_ptr(&(se3_security_info.sessions), sid);
    if (ctx == NULL) {
        // this should not happen
        SE3_TRACE(("[crypto_init] NULL session pointer\n"));
        return SE3_ERR_HW;
    }

    status = handler(&key, req_params.mode, ctx);

    if (SE3_OK != status) {
        // free the allocated session
        se3_mem_free(&(se3_security_info.sessions), (int32_t)resp_params.sid);

        SE3_TRACE(("[crypto_init] crypto handler failed\n"));
        return status;
    }

    // link session to algo
    se3_security_info.sessions_algo[resp_params.sid] = req_params.algo;

    SE3_SET32(resp, SE3_CMD1_CRYPTO_INIT_RESP_OFF_SID, resp_params.sid);

    *resp_size = SE3_CMD1_CRYPTO_INIT_RESP_SIZE;

	return SE3_OK;
}

/** \brief use a crypto context
 *
 *  crypto_update : (
 *      sid:ui32, flags:ui16, datain1-len:ui16, datain2-len:ui16, pad-to-16[6],
 *      datain1[datain1-len], pad-to-16[...], datain2[datain2-len])
 *  => (dataout-len, pad-to-16[14], dataout[dataout-len])
 */
uint16_t crypto_update(uint16_t req_size, const uint8_t* req, uint16_t* resp_size, uint8_t* resp)
{
    struct {
        uint32_t sid;
        uint16_t flags;
        uint16_t datain1_len;
        uint16_t datain2_len;
        const uint8_t* datain1;
        const uint8_t* datain2;
    } req_params;
    struct {
        uint16_t dataout_len;
        uint8_t* dataout;
    } resp_params;
    uint16_t datain1_len_padded;
    se3_crypto_update_handler handler = NULL;
    uint16_t algo;
    uint8_t* ctx;
    uint16_t status;

    if (req_size < SE3_CMD1_CRYPTO_UPDATE_REQ_OFF_DATA) {
        SE3_TRACE(("[crypto_update] req size mismatch\n"));
        return SE3_ERR_PARAMS;
    }

//    if (!se3_security_info.login.y) {
//        SE3_TRACE(("[crypto_update] not logged in\n"));
//        return SE3_ERR_ACCESS;
//    }

    SE3_GET32(req, SE3_CMD1_CRYPTO_UPDATE_REQ_OFF_SID, req_params.sid);
    SE3_GET16(req, SE3_CMD1_CRYPTO_UPDATE_REQ_OFF_FLAGS, req_params.flags);
    SE3_GET16(req, SE3_CMD1_CRYPTO_UPDATE_REQ_OFF_DATAIN1_LEN, req_params.datain1_len);
    SE3_GET16(req, SE3_CMD1_CRYPTO_UPDATE_REQ_OFF_DATAIN2_LEN, req_params.datain2_len);
    req_params.datain1 = req + SE3_CMD1_CRYPTO_UPDATE_REQ_OFF_DATA;
    if (req_params.datain1_len % 16) {
        datain1_len_padded = req_params.datain1_len + (16 - (req_params.datain1_len % 16));
    }
    else {
        datain1_len_padded = req_params.datain1_len;
    }
    req_params.datain2 = req + SE3_CMD1_CRYPTO_UPDATE_REQ_OFF_DATA + datain1_len_padded;

    if (SE3_CMD1_CRYPTO_UPDATE_REQ_OFF_DATA + datain1_len_padded + req_params.datain2_len > SE3_REQ1_MAX_DATA) {
        SE3_TRACE(("[crypto_update] data size exceeds packet limit\n"));
        return SE3_ERR_PARAMS;
    }

    if (req_params.sid >= SE3_SESSIONS_MAX) {
        SE3_TRACE(("[crypto_update] invalid sid\n"));
        return SE3_ERR_RESOURCE;
    }

    algo = se3_security_info.sessions_algo[req_params.sid];
    if (algo >= SE3_ALGO_MAX) {
        SE3_TRACE(("[crypto_update] invalid algo for this sid (wrong sid?)\n"));
        return SE3_ERR_RESOURCE;
    }

    handler = algo_table[algo].update;
    if (handler == NULL) {
        SE3_TRACE(("[crypto_update] invalid crypto handler for this algo (wrong sid?)\n"));
        return SE3_ERR_RESOURCE;
    }

    ctx = se3_mem_ptr(&(se3_security_info.sessions), (int32_t)req_params.sid);
    if (ctx == NULL) {
        SE3_TRACE(("[crypto_update] session not found\n"));
        return SE3_ERR_RESOURCE;
    }

    resp_params.dataout_len = 0;
    resp_params.dataout = resp + SE3_CMD1_CRYPTO_UPDATE_RESP_OFF_DATA;

    status = handler(
        ctx, req_params.flags,
        req_params.datain1_len, req_params.datain1,
        req_params.datain2_len, req_params.datain2,
        &(resp_params.dataout_len), resp_params.dataout);

    if (SE3_OK != status) {
        SE3_TRACE(("[crypto_update] crypto handler failed\n"));
        return status;
    }

    if (req_params.flags & SE3_CRYPTO_FLAG_FINIT) {
        se3_mem_free(&(se3_security_info.sessions), (int32_t)req_params.sid);
    }

    SE3_SET16(resp, SE3_CMD1_CRYPTO_UPDATE_RESP_OFF_DATAOUT_LEN, resp_params.dataout_len);
    *resp_size = SE3_CMD1_CRYPTO_UPDATE_RESP_OFF_DATA + resp_params.dataout_len;

    return SE3_OK;
}

/** \brief set device time for key validity
 *
 *  crypto_set_time : (devtime:ui32) => ()
 */
uint16_t crypto_set_time(uint16_t req_size, const uint8_t* req, uint16_t* resp_size, uint8_t* resp)
{
    struct {
        uint32_t devtime;
    } req_params;

    if (req_size != SE3_CMD1_CRYPTO_SET_TIME_REQ_SIZE) {
        SE3_TRACE(("[crypto_set_time] req size mismatch\n"));
        return SE3_ERR_PARAMS;
    }
//    if (!se3_security_info.login.y) {
//        SE3_TRACE(("[crypto_set_time] not logged in\n"));
//        return SE3_ERR_ACCESS;
//    }

    SE3_GET32(req, SE3_CMD1_CRYPTO_SET_TIME_REQ_OFF_DEVTIME, req_params.devtime);

    se3_time_set((uint64_t)req_params.devtime);

    return SE3_OK;
}

/** \brief get list of available algorithms
 *
 *  crypto_list : () => (count:ui16, algoinfo0, algoinfo1, ...)
 *      algoinfo : (name[16], type:u16, block_size:u16, key_size:u16)
 */
uint16_t crypto_list(uint16_t req_size, const uint8_t* req, uint16_t* resp_size, uint8_t* resp)
{
    struct {
        uint16_t count;
        uint8_t* algoinfo;
    } resp_params;
    uint8_t* p;
    size_t i;
    uint16_t size;

    if (req_size != SE3_CMD1_CRYPTO_LIST_REQ_SIZE) {
        SE3_TRACE(("[crypto_list] req size mismatch\n"));
        return SE3_ERR_PARAMS;
    }
//    if (!se3_security_info.login.y) {
//        SE3_TRACE(("[crypto_list] not logged in\n"));
//        return SE3_ERR_ACCESS;
//    }

    resp_params.algoinfo = resp + SE3_CMD1_CRYPTO_LIST_RESP_OFF_ALGOINFO;

    size = SE3_CMD1_CRYPTO_LIST_RESP_OFF_ALGOINFO;
    resp_params.count = 0;
    p = resp_params.algoinfo;
    for (i = 0; i < SE3_ALGO_MAX; i++) {
        if ((algo_table[i].init != NULL) && (algo_table[i].update != NULL)) {
            memcpy(p + SE3_CMD1_CRYPTO_ALGOINFO_OFF_NAME, algo_table[i].display_name, SE3_CMD1_CRYPTO_ALGOINFO_NAME_SIZE);
            SE3_SET16(p, SE3_CMD1_CRYPTO_ALGOINFO_OFF_TYPE, algo_table[i].display_type);
            SE3_SET16(p, SE3_CMD1_CRYPTO_ALGOINFO_OFF_BLOCK_SIZE, algo_table[i].display_block_size);
            SE3_SET16(p, SE3_CMD1_CRYPTO_ALGOINFO_OFF_KEY_SIZE, algo_table[i].display_key_size);

            (resp_params.count)++;
            size += SE3_CMD1_CRYPTO_ALGOINFO_SIZE;
            p += SE3_CMD1_CRYPTO_ALGOINFO_SIZE;
        }
    }
    SE3_SET16(resp, SE3_CMD1_CRYPTO_LIST_RESP_OFF_COUNT, resp_params.count);
    *resp_size = size;
    return SE3_OK;
}

void se3_payload_cryptoinit(se3_payload_cryptoctx* ctx, const uint8_t* key)
{
	uint8_t keys[2 * B5_AES_256];

	PBKDF2HmacSha256(key, B5_AES_256, NULL, 0, 1, keys, 2 * B5_AES_256);
    B5_Aes256_Init(&(ctx->aesenc), keys, B5_AES_256, B5_AES256_CBC_ENC);
    B5_Aes256_Init(&(ctx->aesdec), keys, B5_AES_256, B5_AES256_CBC_DEC);
	memcpy(ctx->hmac_key, keys + B5_AES_256, B5_AES_256);
	memset(keys, 0, 2 * B5_AES_256);
}

bool se3_payload_encrypt(se3_payload_cryptoctx* ctx, uint8_t* auth, uint8_t* iv, uint8_t* data, uint16_t nblocks, uint16_t flags, uint8_t crypto_algo)
{
	switch(crypto_algo){
		case SE3_AES256:
		    if (flags & SE3_CMDFLAG_ENCRYPT) {
		        B5_Aes256_SetIV(&(ctx->aesenc), iv);
		        B5_Aes256_Update(&(ctx->aesenc), data, data, nblocks);
		    } break;

		case SE3_CRC16:
			//to be implemented

		case SE3_PBKDF2:
			//to be implemented

		case SE3_SHA256:
			//to be implemented

		default: return false; break;
	}

    if (flags & SE3_CMDFLAG_SIGN) {
        B5_HmacSha256_Init(&(ctx->hmac), ctx->hmac_key, B5_AES_256);
        B5_HmacSha256_Update(&(ctx->hmac), iv, B5_AES_IV_SIZE);
        B5_HmacSha256_Update(&(ctx->hmac), data, nblocks*B5_AES_BLK_SIZE);
        B5_HmacSha256_Finit(&(ctx->hmac), ctx->auth);
        memcpy(auth, ctx->auth, 16);
    }
    else {
        memset(auth, 0, 16);
    }
    return true;
}

bool se3_payload_decrypt(se3_payload_cryptoctx* ctx, const uint8_t* auth, const uint8_t* iv, uint8_t* data, uint16_t nblocks, uint16_t flags, uint8_t crypto_algo)
{
    if (flags & SE3_CMDFLAG_SIGN) {
        B5_HmacSha256_Init(&(ctx->hmac), ctx->hmac_key, B5_AES_256);
        B5_HmacSha256_Update(&(ctx->hmac), iv, B5_AES_IV_SIZE);
        B5_HmacSha256_Update(&(ctx->hmac), data, nblocks*B5_AES_BLK_SIZE);
        B5_HmacSha256_Finit(&(ctx->hmac), ctx->auth);
        if (memcmp(auth, ctx->auth, 16)) {
            return false;
        }
    }

	switch(crypto_algo){
		case SE3_AES256:
		    if (flags & SE3_CMDFLAG_ENCRYPT) {
		        B5_Aes256_SetIV(&(ctx->aesdec), iv);
		        B5_Aes256_Update(&(ctx->aesdec), data, data, nblocks);
		    } break;

		case SE3_CRC16:
			//to be implemented

		case SE3_PBKDF2:
			//to be implemented

		case SE3_SHA256:
			//to be implemented

		default: return false; break;
	}


    return true;
}
