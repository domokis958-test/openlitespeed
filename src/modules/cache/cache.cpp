/*****************************************************************************
*    Open LiteSpeed is an open source HTTP server.                           *
*    Copyright (C) 2013 - 2016  LiteSpeed Technologies, Inc.                 *
*                                                                            *
*    This program is free software: you can redistribute it and/or modify    *
*    it under the terms of the GNU General Public License as published by    *
*    the Free Software Foundation, either version 3 of the License, or       *
*    (at your option) any later version.                                     *
*                                                                            *
*    This program is distributed in the hope that it will be useful,         *
*    but WITHOUT ANY WARRANTY; without even the implied warranty of          *
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the            *
*    GNU General Public License for more details.                            *
*                                                                            *
*    You should have received a copy of the GNU General Public License       *
*    along with this program. If not, see http://www.gnu.org/licenses/.      *
*****************************************************************************/
#include "cacheconfig.h"
#include "cachectrl.h"
#include "cacheentry.h"
#include "cachehash.h"
#include "dirhashcachestore.h"

#include <limits.h>
#include <ls.h>
#include <lsr/ls_confparser.h>
#include <util/autostr.h>
#include <util/datetime.h>
#include <util/stringtool.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#include <util/gpath.h>

#include <http/httpserverconfig.h>
#include <http/httpreq.h>
#include <http/httpsession.h>
#include <http/httpvhost.h>
#include <util/autostr.h>



#define MAX_CACHE_CONTROL_LENGTH    128
#define MAX_RESP_HEADERS_NUMBER     50
#define MNAME                       cache
#define ModuleNameStr               "Module-Cache"
#define CACHEMODULEKEY              "_lsi_module_cache_handler__"
#define CACHEMODULEKEYLEN           (sizeof(CACHEMODULEKEY) - 1)
#define CACHEMODULEROOT             "cachedata/"

//The below info should be gotten from the configuration file
#define max_file_len        4096
#define VALMAXSIZE          4096
#define MAX_HEADER_LEN 16384

/////////////////////////////////////////////////////////////////////////////
extern lsi_module_t MNAME;

static const char s_x_cached[] = "X-LiteSpeed-Cache";
static const char *s_hits[3] = { "hit", "hit,private", "hit,litemage" };
static int s_hitsLen[3] = { 3, 11, 12 };

static const char *cache_env_key[] = {"cache-control", "cache-ctrl"};
static int icache_env_key[] = {13, 10};


enum
{
    CE_STATE_NOCACHE = 0,
    CE_STATE_HAS_RIVATECACHE,
    CE_STATE_HAS_PUBLIC_CACHE,
    CE_STATE_WILLCACHE,
    CE_STATE_CACHED,
    CE_STATE_CACHEFAILED,
};

enum HTTP_METHOD
{
    HTTP_UNKNOWN = 0,
    HTTP_OPTIONS,
    HTTP_GET ,
    HTTP_HEAD,
    HTTP_POST,
    HTTP_PUT ,
    HTTP_DELETE,
    HTTP_TRACE,
    HTTP_CONNECT,
    HTTP_MOVE,
    DAV_PROPFIND,
    DAV_PROPPATCH,
    DAV_MKCOL,
    DAV_COPY,
    DAV_LOCK,
    DAV_UNLOCK,
    DAV_VERSION_CONTROL,
    DAV_REPORT,
    DAV_CHECKIN,
    DAV_CHECKOUT,
    DAV_UNCHECKOUT,
    DAV_UPDATE,
    DAV_MKWORKSPACE,
    DAV_LABEL,
    DAV_MERGE,
    DAV_BASELINE_CONTROL,
    DAV_MKACTIVITY,
    DAV_BIND,
    DAV_SEARCH,
    HTTP_PURGE,
    HTTP_REFRESH,
    HTTP_METHOD_END,
};


struct MyMData
{
    CacheConfig *pConfig;
    CacheEntry  *pEntry;
    char        *pOrgUri;
    AutoStr2    *pCacheVary;
    unsigned char iCacheState;
    unsigned char iMethod;
    unsigned char iHaveAddedHook;
    unsigned char iCacheSendBody;
    CacheCtrl cacheCtrl;
    CacheHash cePublicHash;
    CacheHash cePrivateHash;
    CacheKey  cacheKey;
    int hkptIndex;
};

const int paramArrayCount = 17;
const char *paramArray[paramArrayCount + 1] =
{
    "enableCache",   //0
    "qsCache",
    "reqCookieCache",
    "respCookieCache",
    "ignoreReqCacheCtrl",
    "ignoreRespCacheCtrl", //5
    "expireInSeconds",
    "maxStaleAge",
    "enablePrivateCache",
    "privateExpireInSeconds",
    "storagepath", //10 "cacheStorePath"

    //Below 5 are newly added
    "checkPrivateCache", //11
    "checkPublicCache",
    "maxCacheObjSize",
    "noCacheDomain",
    "noCacheUrl",
    
    "no-vary", //16
    NULL //Must have NULL in the last item
};


//Update permission of the dest to the same of the src
static void matchDirectoryPermissions(const char *src, const char *dest)
{
    struct stat st;
    if (stat(src, &st) != -1)
        chown(dest, st.st_uid, st.st_gid);
}


static int createCachePath(const char *path, int mode)
{
    struct stat st;
    if (stat(path, &st) == -1)
    {
        if (GPath::createMissingPath((char *)path, mode) == -1)
            return LS_FAIL;
    }
    return LS_OK;
}


static int parseStoragePath(CacheConfig *pConfig, const char *pValStr, 
                            int valLen, int level, const char *name)
{
    if (level != LSI_CFG_CONTEXT)
    {
        char *pBak = new char[valLen + 1];
        strncpy(pBak, pValStr, valLen);
        pBak[valLen] = 0x00;
        pValStr = pBak;

        char pTmp[max_file_len]  = {0};
        char cachePath[max_file_len]  = {0};

        //check if contains $
        if (strchr(pValStr, '$'))
        {
            int ret = g_api->expand_current_server_varible(level, pValStr, pTmp,
                      max_file_len);
            if (ret >= 0)
            {
                pValStr = pTmp;
                valLen = ret;
            }
            else
            {
                g_api->log(NULL, LSI_LOG_ERROR,
                           "[%s]parseConfig failed to expand_current_server_varible[%s], default will be in use.\n",
                           ModuleNameStr, pValStr);

                delete []pBak;
                return -1;
            }
        }

        if (pValStr[0] != '/')
            strcpy(cachePath, g_api->get_server_root());
        strncat(cachePath, pValStr, valLen);
        strncat(cachePath, "/", 1);
        
        if (createCachePath(cachePath, 0755) == -1)
        {
            g_api->log(NULL, LSI_LOG_ERROR,
                       "[%s]parseConfig failed to create directory [%s].\n",
                       ModuleNameStr, cachePath);
        }
        else
        {
            char defaultCachePath[max_file_len];
            strcpy(defaultCachePath, g_api->get_server_root());
            strncat(defaultCachePath, CACHEMODULEROOT, strlen(CACHEMODULEROOT));
            matchDirectoryPermissions(defaultCachePath, cachePath);
            pConfig->setStore(new DirHashCacheStore);
            pConfig->getStore()->setStorageRoot(cachePath);
            pConfig->getStore()->initManager();
            pConfig->setOwnStore(1);
            g_api->log(NULL, LSI_LOG_DEBUG,
                       "[%s]parseConfig setStoragePath [%s] for level %d[name: %s].\n",
                       ModuleNameStr, cachePath, level, name);
        }
        delete []pBak;
    }
    else
        g_api->log(NULL, LSI_LOG_INFO,
                   "[%s]context [%s] shouldn't have 'storagepath' parameter.\n",
                   ModuleNameStr, name);

    return 0;
}


static int parseNoCacheDomain(CacheConfig *pConfig, const char *pValStr, 
                            int valLen, int level, const char *name)
{
    if (level != LSI_CFG_SERVER)
    {
        g_api->log(NULL, LSI_LOG_INFO,
                   "[%s][%s] Only SERVER level can have 'noCacheDomain' parameter.\n",
                   ModuleNameStr, name);
        return 0;
    }
    
    if (!pConfig->getVHostMapExclude())
        pConfig->setVHostMapExclude(new VHostMap());
    pConfig->getVHostMapExclude()->mapDomainList(
        HttpServerConfig::getInstance().getGlobalVHost(), pValStr);

    return 0;
}


static int parseNoCacheUrl(CacheConfig *pConfig, const char *pValStr, 
                            int valLen, int level, const char *name)
{
    if (level != LSI_CFG_SERVER && level != LSI_CFG_VHOST)
    {
        g_api->log(NULL, LSI_LOG_INFO,
                   "[%s][%s] Only SERVER and VHOST level can have 'noCacheUrl' parameter.\n",
                   ModuleNameStr, name);
        return 0;
    }

    bool bClear = (valLen == 7 && strncasecmp(pValStr, "<clear>", 7) == 0);
    if (bClear)
        pConfig->setOnlyUseOwnUrlExclude(1);
    
    if (!pConfig->getUrlExclude())
        pConfig->setUrlExclude(new Aho(1));

    pConfig->getUrlExclude()->addPattern(pValStr, valLen);
    return 0;
}

void parseNoCacheUrlFinal(CacheConfig *pConfig)
{
    if (pConfig->getUrlExclude())
    {
        pConfig->getUrlExclude()->makeTree();
        pConfig->getUrlExclude()->optimizeTree();
    }
}


// Parses the key and value given.  If key is storagepath, returns 1, otherwise returns 0
static int parseLine(CacheConfig *pConfig, const char *key, int keyLen,
                     const char *val, int valLen)
{
    int i;
    int bit;
    int minValue = 0;
    int maxValue = 1;
    int defValue = 0;
    for (i = 0; i < paramArrayCount; ++i)
    {
        if (strncasecmp(key, paramArray[i], keyLen) == 0)
            break;
    }
    if (i == paramArrayCount)
    {
        //compatible with lslb
        if (strncasecmp(key, "cacheStorePath", keyLen) == 0)
            i = 10;
    }

    switch (i)
    {
    case 0:
        bit = CACHE_ENABLED;
        break;
    case 1:
        bit = CACHE_QS_CACHE;
        break;
    case 2:
        bit = CACHE_REQ_COOKIE_CACHE;
        break;
    case 3:
        bit = CACHE_RESP_COOKIE_CACHE;
        break;
    case 4:
        bit = CACHE_IGNORE_REQ_CACHE_CTRL_HEADER;
        break;
    case 5:
        bit = CACHE_IGNORE_RESP_CACHE_CTRL_HEADER;
        break;
    case 6:
        bit = CACHE_MAX_AGE_SET;
        maxValue = INT_MAX;
        break;
    case 7:
        bit = CACHE_STALE_AGE_SET;
        maxValue = INT_MAX;
        break;
    case 8:
        bit = CACHE_PRIVATE_ENABLED;
        break;
    case 9:
        bit = CACHE_PRIVATE_AGE_SET;
        maxValue = INT_MAX;
        break;
    case 10:
    case 14:
    case 15:
        return i; //return the index for next step parsing

    case 11:
        bit = CACHE_CHECK_PRIVATE;
        break;
    case 12:
        bit = CACHE_CHECK_PUBLIC;
        break;
    case 13:
        bit = CACHE_MAX_OBJ_SIZE;
        maxValue = INT_MAX;
        defValue = 1024 * 1024;
        break;
    case 16:
        bit = CACHE_NO_VARY;
        break;

    default:
        return 0;
    }

    int64_t value = strtoll(val, NULL, 10);

    if (value < minValue || value > maxValue)
        value = defValue;

    if (bit == CACHE_MAX_AGE_SET)
        pConfig->setDefaultAge(value);
    else if (bit == CACHE_STALE_AGE_SET)
        pConfig->setMaxStale(value);
    else if (bit == CACHE_PRIVATE_AGE_SET)
        pConfig->setPrivateAge(value);
    else if (bit == CACHE_MAX_OBJ_SIZE)
        pConfig->setMaxObjSize(value);

    pConfig->setConfigBit(bit, value);
    return 0;
}


static void verifyStoreReady(CacheConfig *pConfig)
{
    if (pConfig->getStore())
        return ;

    pConfig->setStore(new DirHashCacheStore);
    if (pConfig->getStore())
    {
        char defaultCachePath[max_file_len];
        strcpy(defaultCachePath, g_api->get_server_root());
        strncat(defaultCachePath, CACHEMODULEROOT, strlen(CACHEMODULEROOT));
        pConfig->getStore()->setStorageRoot((const char *)defaultCachePath);
        pConfig->getStore()->initManager();
        pConfig->setOwnStore(1);
    }
    else
        g_api->log(NULL, LSI_LOG_ERROR,
                   "Cache verifyStoreReady failed to alloc memory.\n");
}

static void matchSettings(const char *param, int param_len,CacheConfig *pConfig)
{
    //need to tune the setting of enbleCache and CheckPublic
    if (!strcasestr(param, "checkPublicCache"))
        pConfig->setConfigBit(CACHE_CHECK_PUBLIC,
                              pConfig->isSet(CACHE_ENABLED));
        
   if (!strcasestr(param, "checkPrivateCache"))
        pConfig->setConfigBit(CACHE_CHECK_PRIVATE,
                              pConfig->isSet(CACHE_PRIVATE_ENABLED));
}



static void *ParseConfig(const char *param, int param_len,
                         void *_initial_config, int level, const char *name)
{
    const char *pLineBegin;
    const char *pLineEnd;
    const char *pParamOrg = param;
    const char *pParamEnd = param + param_len;
    ls_confparser_t cp;
    ls_objarray_t *pList;
    CacheConfig *pInitConfig = (CacheConfig *)_initial_config;
    CacheConfig *pConfig = new CacheConfig;
    if (!pConfig)
        return NULL;

    pConfig->setLevel(level);
    pConfig->inherit(pInitConfig);
    if (!param)
    {
        verifyStoreReady(pConfig);
        return (void *)pConfig;
    }

    ls_confparser(&cp);

    while ((pLineBegin = ls_getconfline(&param, pParamEnd, &pLineEnd)) != NULL)
    {
        pList = ls_confparser_linekv(&cp, pLineBegin, pLineEnd);
        if (!pList)
            continue;

        ls_str_t *pKey = (ls_str_t *)ls_objarray_getobj(pList, 0);
        ls_str_t *pVal = (ls_str_t *)ls_objarray_getobj(pList, 1);
        const char *pValStr = ls_str_cstr(pVal);
        if (pValStr == NULL)
            continue;

        int valLen = ls_str_len(pVal);
        if (valLen == 0)
            continue;

        int ret = parseLine(pConfig, ls_str_cstr(pKey), ls_str_len(pKey), pValStr,
                      valLen);
        
        if (ret == 10)
            parseStoragePath(pConfig, pValStr, valLen, level, name);
        else if (ret == 15)
            parseNoCacheUrl(pConfig, pValStr, valLen, level, name);
        else if (ret == 14)
            parseNoCacheDomain(pConfig, pValStr, valLen, level, name);
    }

    parseNoCacheUrlFinal(pConfig);

    //Only server level need to match CHECKXXXX seetings
    if(level == 0)
        matchSettings(pParamOrg, param_len, pConfig);

    ls_confparser_d(&cp);
    verifyStoreReady(pConfig);
    return (void *)pConfig;
}


static void FreeConfig(void *_config)
{
    delete(CacheConfig *)_config;
}


static int isUrlExclude(lsi_session_t *session, CacheConfig *pConfig,
                        const char * url, int urlLen)
{
    Aho *ahos[2] = {pConfig->getUrlExclude(), pConfig->getParentUrlExclude()};
    int count = 2;
    if (pConfig->isOnlyUseOwnUrlExclude())
        count = 1;
    
    for (int i=0; i<count ; ++i)
    {
        if (ahos[i])
        {
            size_t out_start, out_end;
            AhoState *out_last_state;
            int ret = ahos[i]->search(NULL, url, urlLen, 0,
                                      &out_start, &out_end,
                                      &out_last_state);
            if (ret)
            {
                g_api->log(session, LSI_LOG_INFO, "Cache excluded by URL: %.*s\n", 
                           urlLen, url);
                return 1;
            }
        }
    }
    return 0;
}


static int isDomainExclude(lsi_session_t *session, CacheConfig *pConfig)
{
    if (pConfig->getVHostMapExclude())
    {
        HttpSession *pSession = (HttpSession *)session;
        HttpReq *pReq =pSession->getReq();
        const HttpVHost *pVHost = pReq->matchVHost(pConfig->getVHostMapExclude());
        if (pVHost)
        {
            g_api->log(session, LSI_LOG_INFO, "Cache excluded by domain.\n");
            return 1;
        }
    }
    return 0;
}

static char *copyCookie(char *p, char *pDestEnd,
                        cookieval_t *pIndex, const char *pCookie)
{
    int n = pIndex->valLen + pIndex->valOff - pIndex->keyOff;
    if (n > (pDestEnd - p - 1))
        n = pDestEnd - p - 1;
    if (n > 0)
    {
        memmove(p, pCookie, n);
        p += n;
        *p++ = ';';
    }
    return p;
}

int getPrivateCacheCookie(HttpReq *pReq, char *pDest, char *pDestEnd)
{
    cookieval_t *pIndex;
    pReq->parseCookies();
    if (pReq->getCookieList().getSize() == 0)
    {
        *pDest = 0;
        return 0;
    }

    char *p = pDest;
    if (pReq->getCookieList().isSessIdxSet())
    {
        pIndex = pReq->getCookieList().getObj(pReq->getCookieList().getSessIdx());
        //assert( (pIndex->flag & COOKIE_FLAG_SESSID) != 0 );
        p = copyCookie(p, pDestEnd, pIndex,
                       pReq->getHeaderBuf().getp(pIndex->keyOff));
        *p = 0;
        return p - pDest;
    }
    pIndex = pReq->getCookieList().begin();
    const char *pCookie;
    int skip;
    while ((pIndex < pReq->getCookieList().end()) && (p < pDestEnd))
    {
        pCookie = pReq->getHeaderBuf().getp(pIndex->keyOff);
        skip = 0;
        if ((*pCookie == '_') && (*(pCookie + 1) == '_'))
            skip = 1;
        else if ((strncmp("has_js=", pCookie, 7) == 0)
                 || (strncmp("_lscache_vary", pCookie, 13) == 0)
                 || (strncmp("bb_forum_view=", pCookie, 14) == 0))
            skip = 1;
        else if (strncmp("frontend=", pCookie, 9) == 0)
        {
            int n = pIndex->valLen + pIndex->keyLen + 2;
            if ((p - pDest >= n)
                && (memcmp(pCookie, p - n, n - 1) == 0))
                skip = 1;
        }
        if (!skip)
            p = copyCookie(p, pDestEnd, pIndex, pCookie);
        ++pIndex;
    }
    *p = '\0';
    return p - pDest;
}


char *appendVaryCookie(HttpReq *pReq, const char *pCookeName, int len,
                                char * pDest, char *pDestEnd)
{
    cookieval_t *pIndex = pReq->getCookie(pCookeName, len);
    if (pIndex)
    {
        const char *pCookie = pReq->getHeaderBuf().getp(pIndex->keyOff);
        pDest = copyCookie(pDest, pDestEnd, pIndex, pCookie);
    }
    return pDest;
}


char * scanVaryOnList(HttpReq *pReq, const char *pListBegin, 
                      const char * pListEnd, char * pDest, char *pDestEnd)
{
    while (pListBegin < pListEnd)
    {
        const char *pVary = pListBegin;
        while( pVary < pListEnd && isspace(*pVary) )
            ++pVary;
        if (strncasecmp(pVary, "cookie=", 7) == 0)
            pVary += 7;
        const char *pVaryEnd = strchr(pVary, ',');
        if (!pVaryEnd)
        {
            pVaryEnd = pListEnd;
            pListBegin = pListEnd;
        }
        else
            pListBegin = pVaryEnd + 1;
        if (pVaryEnd - pVary > 0)
        {
            pDest = appendVaryCookie(pReq, pVary, pVaryEnd - pVary,
                                     pDest, pDestEnd);
        }
    }
    return pDest;
}


int getCacheVaryCookie(lsi_session_t *session, HttpReq *pReq, char *pDest, char *pDestEnd)
{
    char achCacheCtrl[8192];
    pReq->parseCookies();
    if (pReq->getCookieList().getSize() == 0)
    {
        *pDest = 0;
        return 0;
    }
    char *p = pDest;
    p = appendVaryCookie(pReq, "_lscache_vary", 13, p, pDestEnd);

    const char *pVaryListBegin = NULL;
    const char *pVaryListEnd = NULL;

    MyMData *myData = (MyMData *)g_api->get_module_data(session, &MNAME,
                      LSI_DATA_HTTP);

    if (myData && myData->pCacheVary)
    {
        pVaryListBegin = myData->pCacheVary->c_str();
        pVaryListEnd = myData->pCacheVary->c_str() + myData->pCacheVary->len();
        p = scanVaryOnList(pReq, pVaryListBegin, pVaryListEnd, p, pDestEnd);
    }

    int len = g_api->get_req_env(session, "cache-control", 13, achCacheCtrl, 8191);
    if (len == 0)
        len = g_api->get_req_env(session, "cache-ctrl", 10, achCacheCtrl, 8191);
    
    if (len > 5 && strncasecmp(achCacheCtrl, "vary=", 5) == 0)
    {
        achCacheCtrl[len] = 0x00;
        p = scanVaryOnList(pReq, achCacheCtrl + 5, achCacheCtrl + len, p, pDestEnd);
    }
    //should save to cookie
    len = g_api->get_req_env(session, "cache-Vary", 10, achCacheCtrl, 8191);
    {
        achCacheCtrl[len] = 0x00;
        p = scanVaryOnList(pReq, achCacheCtrl + 5, achCacheCtrl + len, p, pDestEnd);
    }
    
    *p = '\0';
    return p - pDest;
}


void calcCacheHash2(lsi_session_t *session,CacheKey *pKey,
                   CacheHash *pHash, CacheHash *pPrivateHash)
{
//    CACHE_PRIVATE_KEY;
    XXH64_state_t state;
    int len;
    char env[512];
    const char *host = g_api->get_req_header_by_id(session, LSI_HDR_HOST, &len);
    XXH64_reset(&state, 0);
    XXH64_update(&state, host, len);

    len = g_api->get_req_var_by_id(session, LSI_VAR_SSL_VERSION, env, 12);
    if( len > 3 ) //SSL
        XXH64_update(&state, "!", 1);
    
    XXH64_update(&state, pKey->m_pUri, pKey->m_iUriLen);
    
    if (pKey->m_iQsLen > 0)
    {
        XXH64_update(&state, "?", 1);
        XXH64_update(&state, pKey->m_pQs, pKey->m_iQsLen);
    }

    if (pKey->m_iCookieVary > 0)
    {
        XXH64_update(&state, "#", 1);
        XXH64_update(&state, pKey->m_sCookie.c_str(), pKey->m_iCookieVary);
    }
    pHash->setKey(XXH64_digest(&state));

    if (pKey->m_pIP)
    {
        if (pKey->m_iCookiePrivate > 0)
        {
            XXH64_update(&state, "~", 1);
            XXH64_update(&state, pKey->m_sCookie.c_str() + pKey->m_iCookieVary,
                         pKey->m_iCookiePrivate);
        }
        XXH64_update(&state, "@", 1);
        XXH64_update(&state, pKey->m_pIP, pKey->m_ipLen);
        pPrivateHash->setKey(XXH64_digest(&state));
    }
}


void calcCacheHash(lsi_session_t *session, CacheKey *pKey, 
                   CacheHash *pHash, CacheHash *pPrivateHash)
{
    HttpSession *pSession = (HttpSession *)session;
    HttpReq *pReq =pSession->getReq();
    if (pReq->getContextState(CACHE_PRIVATE_KEY))
        return;
    if ((!pKey->m_pIP) && (pReq->getContextState(CACHE_KEY)))
        return;
    calcCacheHash2(session, pKey, pHash, pPrivateHash);
    if (pKey->m_pIP)
        pReq->orContextState(CACHE_PRIVATE_KEY | CACHE_KEY);
    else
        pReq->orContextState(CACHE_KEY);
}



void buildCacheKey(lsi_param_t *rec, const char *uri, int uriLen, int noVary, CacheKey *pKey)
{
    int iQSLen;
    int ipLen;
    char pCookieBuf[MAX_HEADER_LEN] = {0};
    char *pCookieBufEnd = pCookieBuf + MAX_HEADER_LEN;
    const char *pIp = g_api->get_client_ip(rec->session, &ipLen);
    const char *pQs = g_api->get_req_query_string(rec->session, &iQSLen);

    HttpSession *pSession = (HttpSession *)rec->session;
    HttpReq *pReq =pSession->getReq();
    
    pKey->m_pIP = pIp;
    pKey->m_ipLen = ipLen;
    if (noVary)
        pKey->m_iCookieVary = 0;
    else 
        pKey->m_iCookieVary = getCacheVaryCookie(rec->session, pReq, pCookieBuf,
                                                 pCookieBufEnd);
    
    if (pIp)
        pKey->m_iCookiePrivate = getPrivateCacheCookie(pReq,
                                                       &pCookieBuf[pKey->m_iCookieVary], 
                                                       pCookieBufEnd);
    else
        pKey->m_iCookiePrivate = 0;
    pKey->m_pUri = uri;
    pKey->m_iUriLen = uriLen;
    pKey->m_pQs = pQs;
    pKey->m_iQsLen = iQSLen;
    pKey->m_sCookie.setStr(pCookieBuf);
}


short hasCache(lsi_param_t *rec, MyMData *myData,
               int no_vary,
               const char *uri, int uriLen,
               DirHashCacheStore *pDirHashCacheStore,
               CacheHash *cePublicHash, CacheHash *cePrivateHash,
               CacheConfig *pConfig, CacheEntry **pEntry, bool doPublic)
{
    buildCacheKey(rec, uri, uriLen, no_vary,
                   &myData->cacheKey);
    calcCacheHash(rec->session, &myData->cacheKey, cePublicHash, cePrivateHash);

    //Try private one first
    long lastCacheFlush = (long)g_api->get_module_data(rec->session, &MNAME,
                          LSI_DATA_IP);
    *pEntry = pDirHashCacheStore->getCacheEntry(*cePrivateHash, &myData->cacheKey,
                                                lastCacheFlush,
                                                pConfig->getMaxStale());
    if (*pEntry && (!(*pEntry)->isStale() || (*pEntry)->isUpdating()))
        return CE_STATE_HAS_RIVATECACHE;

    if (doPublic)
    {
        //Second, check if can use public one
        *pEntry = pDirHashCacheStore->getCacheEntry(*cePublicHash, &myData->cacheKey, 
                                                    0, pConfig->getMaxStale());
        if (*pEntry && (!(*pEntry)->isStale() || (*pEntry)->isUpdating()))
            return CE_STATE_HAS_PUBLIC_CACHE;
    }

    if (*pEntry)
        (*pEntry)->setStale(1);

    return CE_STATE_NOCACHE;
}


int httpRelease(void *data)
{
    MyMData *myData = (MyMData *)data;
    if (myData)
    {
        if (myData->pOrgUri)
            delete []myData->pOrgUri;
        delete myData;
    }
    return 0;
}


int checkBypassHeader(const char *header, int len)
{
    const char *headersBypass[] =
    {
        "last-modified",
        "etag",
        "date",
        "content-length",
        "transfer-encoding",
        "content-encoding",
    };
    int8_t headersBypassLen[] = {  13, 4, 4, 14, 17, 16, };

    int count = sizeof(headersBypass) / sizeof(const char *);
    for (int i = 0; i < count ; ++i)
    {
        if (len == headersBypassLen[i]
            && strncasecmp(headersBypass[i], header, headersBypassLen[i]) == 0)
            return 1;
    }

    return 0;
}


int writeHttpHeader(int fd, AutoStr2 *str, const char *key, int key_len,
                    const char *val, int val_len)
{
    write(fd, key, key_len);
    write(fd, ": ", 2);
    write(fd, val, val_len);
    write(fd, "\r\n", 2);

#ifdef CACHE_RESP_HEADER
    str->append(key, key_len);
    str->append(": ", 2);
    str->append(val, val_len);
    str->append("\r\n", 2);

#endif

    return key_len + val_len + 4;
}

static void parseEnv(lsi_session_t *session, CacheCtrl &cacheCtrl)
{
    for (int i=0; i<2; ++i)
    {
        char cacheEnv[MAX_CACHE_CONTROL_LENGTH] = {0};
        int cacheEnvLen = g_api->get_req_env(session, cache_env_key[i],
                                             icache_env_key[i],
                                             cacheEnv,
                                             MAX_CACHE_CONTROL_LENGTH);
        if (cacheEnvLen > 0)
            cacheCtrl.parse((const char *)cacheEnv, cacheEnvLen);
    }
}

void getRespHeader(lsi_session_t *session, int header_index, char **buf,
                   int *length)
{
    struct iovec iov[1] = {{NULL, 0}};
    int iovCount = g_api->get_resp_header(session, header_index, NULL, 0, iov,
                                          1);
    if (iovCount == 1)
    {
        *buf = (char *)iov[0].iov_base;
        *length = iov[0].iov_len;
    }
    else
    {
        *buf = NULL;
        *length = 0;
    }
}

void clearHooksOnly(lsi_session_t *session)
{
    int aEnableHkpts[4], iEnableCount = 0;
    MyMData *myData = (MyMData *) g_api->get_module_data(session, &MNAME,
                      LSI_DATA_HTTP);
    if (myData)
    {
        if (myData->iHaveAddedHook == 1 || myData->iHaveAddedHook == 2)
        {
            if (myData->iHaveAddedHook == 2)
                aEnableHkpts[iEnableCount++] = myData->hkptIndex;
            aEnableHkpts[iEnableCount++] = LSI_HKPT_RCVD_RESP_HEADER;
            g_api->enable_hook(session, &MNAME, 0, aEnableHkpts, iEnableCount);
            myData->iHaveAddedHook = 0;
        }
    }
}


void clearHooks(lsi_session_t *session)
{
    clearHooksOnly(session);
    g_api->free_module_data(session, &MNAME, LSI_DATA_HTTP, httpRelease);
}

static int cacheReady(lsi_param_t *rec, MyMData *myData)
{
    if (myData)
    {
        if (myData->pEntry && myData->iCacheState == CE_STATE_WILLCACHE)
        {
            myData->pConfig->getStore()->publish(myData->pEntry);
            myData->iCacheState = CE_STATE_CACHED;  //Succeed
        }
    }
    return 0;
}


static int cancelCache(lsi_param_t *rec)
{
    MyMData *myData = (MyMData *)g_api->get_module_data(rec->session, &MNAME,
                      LSI_DATA_HTTP);
    if (myData != NULL && myData->iCacheState == CE_STATE_WILLCACHE)
        myData->pConfig->getStore()->cancelEntry(myData->pEntry, 1);
    clearHooks(rec->session);
    g_api->log(rec->session, LSI_LOG_DEBUG, "[%s]cache ended.\n",
               ModuleNameStr);
    return 0;
}

static int endCache(lsi_param_t *rec)
{
    MyMData *myData = (MyMData *)g_api->get_module_data(rec->session, &MNAME,
                      LSI_DATA_HTTP);
    if (myData)
    {
        if (myData->hkptIndex == LSI_HKPT_SEND_RESP_BODY)
        {
            char path[4096];
            int cur_uri_len;
            struct stat sb;
            const char *cur_uri = g_api->get_req_uri(rec->session, &cur_uri_len);
            g_api->get_file_path_by_uri(rec->session, cur_uri,cur_uri_len,
                                        path, 4096);
            if (stat(path, &sb) != -1 &&
                sb.st_size == myData->pEntry->getPart2Len())
            {
                //Check if file optimized, if not, do not store it
                cancelCache(rec);
            }
            else
                cacheReady(rec, myData);
        }
        return cancelCache(rec);
    }
    else
        return 0;
}

static int createEntry(lsi_param_t *rec)
{
    MyMData *myData = (MyMData *)g_api->get_module_data(rec->session, &MNAME,
                      LSI_DATA_HTTP);
    if (myData == NULL)
    {
        clearHooks(rec->session);
        g_api->log(rec->session, LSI_LOG_ERROR,
                   "[%s]internal error during createEntry.\n", ModuleNameStr);
        return 0;
    }

    //Error page won't be stored to cache
    int code = g_api->get_status_code(rec->session);
    if (code != 200)
    {
        clearHooks(rec->session);
        g_api->log(rec->session, LSI_LOG_DEBUG,
                   "[%s]cacheTofile to be cancelled for error page, code=%d.\n",
                   ModuleNameStr, code);
        return 0;
    }

    myData->hkptIndex = LSI_HKPT_RCVD_RESP_BODY;
    const char *phandlerType = g_api->get_req_handler_type(rec->session);
    if (phandlerType && memcmp("static", phandlerType, 6) == 0)
    {
        int flag1 = g_api->get_hook_flag(rec->session, LSI_HKPT_RECV_RESP_BODY);
        int flag2 = g_api->get_hook_flag(rec->session, LSI_HKPT_SEND_RESP_BODY);
        if (!(flag1 & (LSI_FLAG_TRANSFORM | LSI_FLAG_PROCESS_STATIC)) &&
            !(flag2 & (LSI_FLAG_TRANSFORM | LSI_FLAG_PROCESS_STATIC)))
        {
            clearHooks(rec->session);
            g_api->log(rec->session, LSI_LOG_DEBUG,
                   "[%s]cacheTofile to be cancelled for static file type.\n",
                   ModuleNameStr);
            return 0;
        }

        if (flag2 & (LSI_FLAG_TRANSFORM | LSI_FLAG_PROCESS_STATIC))
            myData->hkptIndex = LSI_HKPT_SEND_RESP_BODY;
    }

    if (!myData->pConfig->isSet(CACHE_RESP_COOKIE_CACHE))
    {
        struct iovec iov[1] = {{NULL, 0}};
        int iovCount = g_api->get_resp_header(rec->session,
                                              LSI_RSPHDR_SET_COOKIE, NULL,
                                              0, iov, 1);
        if (iov[0].iov_len > 0 && iovCount == 1)
        {
            clearHooks(rec->session);
            g_api->log(rec->session, LSI_LOG_DEBUG,
                       "[%s]cacheTofile to be cancelled for having respcookie.\n",
                       ModuleNameStr);
            return 0;
        }
    }


    if (myData->cacheCtrl.isPrivateCacheable())
        myData->pEntry = myData->pConfig->getStore()->createCacheEntry(
                             myData->cePrivateHash, &myData->cacheKey, 0);
    else// if (myData->cacheCtrl.isPublicCacheable())
        myData->pEntry = myData->pConfig->getStore()->createCacheEntry(
                             myData->cePublicHash, &myData->cacheKey, 0);

    if (myData->pEntry == NULL)
    {
        clearHooks(rec->session);
        g_api->log(rec->session, LSI_LOG_ERROR,
                   "[%s] createEntry failed, code [%d].\n", ModuleNameStr);
        return 0;
    }

    if (!myData->pConfig->isSet(CACHE_IGNORE_RESP_CACHE_CTRL_HEADER))
    {
        iovec iov[3];
        int count = g_api->get_resp_header(rec->session,
                                           LSI_RSPHDR_CACHE_CTRL, NULL, 0, iov, 3);
        for (int i = 0; i < count; ++i)
            myData->cacheCtrl.parse((char *)iov[i].iov_base, iov[i].iov_len);
    }

    //parseEnv(rec->session, myData->cacheCtrl);
    
    //Now we can store it
    myData->iCacheState = CE_STATE_WILLCACHE;
    g_api->enable_hook(rec->session, &MNAME, 1, &myData->hkptIndex, 1);
    myData->iHaveAddedHook = 2;
    return 0;
}


int cacheHeader(lsi_param_t *rec, MyMData *myData)
{
    myData->pEntry->setMaxStale(myData->pConfig->getMaxStale());
    g_api->log(rec->session, LSI_LOG_DEBUG,
               "[%s]save to %s cachestore by cacheHeader(), uri:%s\n", ModuleNameStr,
               ((myData->cacheCtrl.isPrivateCacheable()) ? "private" : "public"),
               myData->pOrgUri);

    int fd = myData->pEntry->getFdStore();
    char *sLastMod = NULL;
    char *sETag = NULL;
    int nLastModLen = 0;
    int nETagLen = 0;
    int headersBufSize = 0;
    CeHeader &CeHeader = myData->pEntry->getHeader();
    CeHeader.m_tmCreated = (int32_t)DateTime_s_curTime;
    CeHeader.m_tmExpire = CeHeader.m_tmCreated + myData->cacheCtrl.getMaxAge();
    getRespHeader(rec->session, LSI_RSPHDR_LAST_MODIFIED, &sLastMod,
                  &nLastModLen);
    if (sLastMod)
        CeHeader.m_tmLastMod = DateTime::parseHttpTime(sLastMod);

    getRespHeader(rec->session, LSI_RSPHDR_ETAG, &sETag, &nETagLen);
    if (sETag && nETagLen > 0)
    {
        if (nETagLen > VALMAXSIZE)
            nETagLen = VALMAXSIZE;
        CeHeader.m_offETag = 0;
        CeHeader.m_lenETag = nETagLen;
    }
    else
        CeHeader.m_lenETag = 0;

    CeHeader.m_statusCode = g_api->get_status_code(rec->session);

    //Right now, set wrong numbers, and this will be fixed when publish
    myData->pEntry->setPart1Len(0); //->setContentLen(0, 0);
    myData->pEntry->setPart2Len(0);

    if (g_api->is_resp_buffer_gzippped(rec->session))
        myData->pEntry->markReady(1);

    myData->pEntry->saveCeHeader();

    if (CeHeader.m_lenETag > 0)
    {
        write(fd, sETag, CeHeader.m_lenETag);

#ifdef CACHE_RESP_HEADER
        myData->m_pEntry->m_sRespHeader.append(sETag, CeHeader.m_lenETag);
#endif
    }

    //Stat to write akll other headers

    int count = g_api->get_resp_headers_count(rec->session);
    if (count >= MAX_RESP_HEADERS_NUMBER)
        g_api->log(rec->session, LSI_LOG_WARN,
                   "[%s] too many resp headers [=%d]\n",
                   ModuleNameStr, count);

    struct iovec iov_key[MAX_RESP_HEADERS_NUMBER];
    struct iovec iov_val[MAX_RESP_HEADERS_NUMBER];
    count = g_api->get_resp_headers(rec->session, iov_key, iov_val,
                                    MAX_RESP_HEADERS_NUMBER);
    for (int i = 0; i < count; ++i)
    {
        //check if need to bypass
        if (!checkBypassHeader((const char *)iov_key[i].iov_base,
                               iov_key[i].iov_len))
#ifdef CACHE_RESP_HEADER
            headersBufSize += writeHttpHeader(fd, &(myData->m_pEntry->m_sRespHeader),
                                              iov_key[i].iov_base, iov_key[i].iov_len,
                                              iov_val[i].iov_base, iov_val[i].iov_len);
#else
            headersBufSize += writeHttpHeader(fd, NULL,
                                              (char *)iov_key[i].iov_base, iov_key[i].iov_len,
                                              (char *)iov_val[i].iov_base, iov_val[i].iov_len);
#endif
    }

#ifdef CACHE_RESP_HEADER
    if (myData->m_pEntry->m_sRespHeader.len() > 4096)
        myData->m_pEntry->m_sRespHeader.prealloc(0);
#endif

    myData->pEntry->setPart1Len(CeHeader.m_lenETag + headersBufSize);
    return 0;
}


int cacheTofile(lsi_param_t *rec)
{
    MyMData *myData = (MyMData *)g_api->get_module_data(rec->session, &MNAME,
                      LSI_DATA_HTTP);
    if (!myData)
        return 0;

    cacheHeader(rec, myData);
    int fd = myData->pEntry->getFdStore();

    long iCahcedSize = 0;
    off_t offset = 0;
    const char *pBuf;
    int len = 0;
    void *pRespBodyBuf = g_api->get_resp_body_buf(rec->session);
    long maxObjSz = myData->pConfig->getMaxObjSize();
    if (maxObjSz > 0 && g_api->get_body_buf_size(pRespBodyBuf) > maxObjSz)
    {
        cancelCache(rec);
        g_api->log(rec->session, LSI_LOG_DEBUG,
           "[%s:cacheTofile] cache cancelled, body buffer size %ld > maxObjSize %ld\n",
           ModuleNameStr, g_api->get_body_buf_size(pRespBodyBuf), maxObjSz);
        return 0;
    }
    
    while (fd != -1 && !g_api->is_body_buf_eof(pRespBodyBuf, offset))
    {
        pBuf = g_api->acquire_body_buf_block(pRespBodyBuf, offset, &len);
        if (!pBuf)
            break;
        write(fd, pBuf, len);
        g_api->release_body_buf_block(pRespBodyBuf, offset);
        offset += len;
        iCahcedSize += len;
    }

    int part1Len = myData->pEntry->getContentTotalLen();
    myData->pEntry->setPart1Len(part1Len);
    myData->pEntry->setPart2Len(iCahcedSize);
    cacheReady(rec, myData);
    g_api->log(rec->session, LSI_LOG_DEBUG,
               "[%s:cacheTofile] stored, size %ld\n",
               ModuleNameStr, offset);
    return 0;
}


int cacheTofileFilter(lsi_param_t *rec)
{
    char cacheEnv[MAX_CACHE_CONTROL_LENGTH] = {0};
    int cacheEnvLen = g_api->get_req_env(rec->session, "cache-control", 13,
                                             cacheEnv, MAX_CACHE_CONTROL_LENGTH);
    if (cacheEnvLen == 8 && strncasecmp(cacheEnv, "no-cache", cacheEnvLen) ==  0)
        return rec->len1;

    //Because Pagespeed module uses the non-blocking way to check if can handle
    //The optimized cache, it will have an eventCb to set the reqVar to notice 
    //cache module to start to cahce, So have to check it here
    MyMData *myData = (MyMData *)g_api->get_module_data(rec->session, &MNAME,
                      LSI_DATA_HTTP);
    if (!myData)
        return rec->len1;
    
    if (myData->iCacheSendBody == 0) //uninit
    {
        myData->iCacheSendBody = 1; //need cache
        cacheHeader(rec, myData);
    }
    int fd = myData->pEntry->getFdStore();
    int part1Len = myData->pEntry->getPart1Len();
    int part2Len = myData->pEntry->getPart2Len();
    int ret = g_api->stream_write_next(rec, (const char *) rec->ptr1,
                                                rec->len1);
    if (ret > 0)
    {
        long maxObjSz = myData->pConfig->getMaxObjSize();
        if (maxObjSz > 0 && part2Len + ret > maxObjSz)
        {
            cancelCache(rec);
            g_api->log(rec->session, LSI_LOG_DEBUG,
               "[%s:cacheTofile] cache cancelled, current size to cache %ld > maxObjSize %ld\n",
               ModuleNameStr, part2Len + ret, maxObjSz);
            return ret;
        }
    
        int len = write(fd, (const char *) rec->ptr1, ret);
        myData->pEntry->setPart1Len(part1Len);
        myData->pEntry->setPart2Len(part2Len + len);
        g_api->log(rec->session, LSI_LOG_DEBUG,
               "[%s:cacheTofileFilter] stored, size %ld\n",
               ModuleNameStr, len);
    }
    return ret; //rec->len1;
}


static int isReqCacheable(lsi_param_t *rec, CacheConfig *pConfig)
{
    if (!pConfig->isSet(CACHE_QS_CACHE))
    {
        int iQSLen;
        const char *pQS = g_api->get_req_query_string(rec->session, &iQSLen);
        if (pQS && iQSLen > 0)
        {
            g_api->log(rec->session, LSI_LOG_DEBUG,
                   "[%s]isReqCacheable return 0 for has QS but qscache disabled.\n",
                   ModuleNameStr);
            return 0;
        }
    }

    if (!pConfig->isSet(CACHE_REQ_COOKIE_CACHE))
    {
        int cookieLen;
        const char *pCookie = g_api->get_req_cookies(rec->session, &cookieLen);
        if (pCookie && cookieLen > 0)
        {
            g_api->log(rec->session, LSI_LOG_DEBUG,
                       "[%s]isReqCacheable return 0 for has reqcookie but reqcookie disabled.\n",
                   ModuleNameStr);
            return 0;
        }
    }

    return 1;
}


// int setCacheUserData(lsi_param_t *rec)
// {
//     const char *p = (char *)rec->ptr1;
//     int lenTotal = rec->len1;
//     const char *pEnd = p + lenTotal;
// 
//     if (lenTotal < 8)
//         return LS_FAIL;
// 
// 
//     //Check the kvPair length matching
//     int len;
//     while (p < pEnd)
//     {
//         memcpy(&len, p, 4);
//         if (len < 0 || len > lenTotal)
//             return LS_FAIL;
// 
//         lenTotal -= (4 + len);
//         p += (4 + len);
//     }
// 
//     if (lenTotal != 0)
//         return LS_FAIL;
// 
// 
//     MyMData *myData = (MyMData *)g_api->get_module_data(rec->session, &MNAME,
//                       LSI_DATA_HTTP);
//     if (myData == NULL) //|| myData->iCacheState != CE_STATE_CACHED)
//         return LS_FAIL;
// 
//     if (rec->len1 < 8)
//         return LS_FAIL;
// 
//     myData->pEntry->m_sPart3Buf.append((char *)rec->ptr1, rec->len1);
// 
//     g_api->log(rec->session, LSI_LOG_DEBUG,
//                "[%s:setCacheUserData] written %d\n", ModuleNameStr,
//                rec->len1);
//     return 0;
// }
// 
// 
// //get the part3 data
// int getCacheUserData(lsi_param_t *rec)
// {
//     MyMData *myData = (MyMData *)g_api->get_module_data(rec->session, &MNAME,
//                       LSI_DATA_HTTP);
//     if (myData == NULL)
//         return LS_FAIL;
// 
// 
// //     char **buf = (char **)rec->_param;
// //     int *len = (int *)(long)rec->_param_len;
// //     *buf = (char *)myData->pEntry->m_sPart3Buf.c_str();
// //     *len = myData->pEntry->m_sPart3Buf.len();
// 
//     return 0;
// }


int getHttpMethod(lsi_param_t *rec, char *httpMethod)
{
    int methodLen = g_api->get_req_var_by_id(rec->session,
                    LSI_VAR_REQ_METHOD, httpMethod, 10);
    int method = HTTP_UNKNOWN;
    switch (methodLen)
    {
    case 3:
        if ((httpMethod[0] | 0x20) == 'g')   //"GET"
            method = HTTP_GET;
        break;
    case 4:
        if (strncasecmp(httpMethod, "HEAD", 4) == 0)
            method = HTTP_HEAD;
        else if (strncasecmp(httpMethod, "POST", 4) == 0)
            method = HTTP_POST;
        break;
    case 5:
        if (strncasecmp(httpMethod, "PURGE", 5) == 0)
            method = HTTP_PURGE;
        break;
    case 7:
        if (strncasecmp(httpMethod, "REFRESH", 7) == 0)
            method = HTTP_REFRESH;
        break;
    default:
        break;
    }

    return method;
}


static int getContronlFlag(CacheConfig *pConfig)
{
    int flag = CacheCtrl::max_age | CacheCtrl::max_stale | CacheCtrl::s_maxage;
    if (pConfig->isSet(CACHE_ENABLED))
        flag |= CacheCtrl::cache_public;
    if (pConfig->isSet(CACHE_PRIVATE_ENABLED))
        flag |= CacheCtrl::cache_private;
    if (pConfig->isSet(CACHE_NO_VARY))
        flag |= CacheCtrl::no_vary;

    if ((flag & CacheCtrl::cache_public) == 0 &&
        (flag & CacheCtrl::cache_private) == 0)
        flag |= CacheCtrl::no_cache;

    return flag;
}



static int checkAssignHandler(lsi_param_t *rec)
{
    int uriLen = g_api->get_req_org_uri(rec->session, NULL, 0);
    if (uriLen <= 0)
    {
        g_api->log(rec->session, LSI_LOG_ERROR,
                   "[%s]checkAssignHandler error 1.\n", ModuleNameStr);
        return 0;
    }

    CacheConfig *pConfig = (CacheConfig *)g_api->get_config(
                               rec->session, &MNAME);
    if (!pConfig)
    {
        g_api->log(rec->session, LSI_LOG_ERROR,
                   "[%s]checkAssignHandler error 2.\n", ModuleNameStr);
        return 0;
    }

    int cur_uri_len;
    const char *cur_uri = g_api->get_req_uri(rec->session, &cur_uri_len);
    if (isUrlExclude(rec->session, pConfig, cur_uri, cur_uri_len))
        return 0;

    if (isDomainExclude(rec->session, pConfig))
        return 0;
    
    char httpMethod[10] = {0};
    int method = getHttpMethod(rec, httpMethod);
    if (method == HTTP_UNKNOWN || method == HTTP_POST)
    {
        //Do not store POST method for handling, only store the timestamp
        if (method == HTTP_POST)
        {
            g_api->set_module_data(rec->session, &MNAME, LSI_DATA_IP,
                                   (void *)(long)DateTime_s_curTime);
        }
        g_api->log(rec->session, LSI_LOG_DEBUG,
                   "[%s]checkAssignHandler returned, method %s[%d].\n",
                   ModuleNameStr, httpMethod, method);
        return 0;
    }

    //If it is range request, quit
    int rangeRequestLen = 0;
    const char *rangeRequest = g_api->get_req_header_by_id(rec->session,
                               LSI_HDR_RANGE, &rangeRequestLen);
    if (rangeRequest && rangeRequestLen > 0)
    {
        g_api->log(rec->session, LSI_LOG_DEBUG,
                   "[%s]checkAssignHandler returned, not support rangeRequest [%s].\n",
                   ModuleNameStr, rangeRequest);
        return 0;
    }

    CacheCtrl cacheCtrl;
    if (method == HTTP_GET || method == HTTP_HEAD)
    {
        int flag = getContronlFlag(pConfig);
        cacheCtrl.init(flag, pConfig->getDefaultAge(), pConfig->getMaxStale());
        parseEnv(rec->session, cacheCtrl);

        if (rec->ptr1 != NULL && rec->len1 > 0)
            cacheCtrl.parse((const char *)rec->ptr1, rec->len1);

        if (!pConfig->isSet(CACHE_IGNORE_REQ_CACHE_CTRL_HEADER))
        {
            int bufLen;
            const char *buf = g_api->get_req_header_by_id(rec->session,
                              LSI_HDR_CACHE_CTRL, &bufLen);
            if (buf && bufLen > 0)
                cacheCtrl.parse(buf, bufLen);
        }


        if (!pConfig->isCheckPublic() && !pConfig->isPrivateCheck())
        {
            if (!isReqCacheable(rec, pConfig))
            {
                clearHooks(rec->session);
                return 0;
            }

//             if (cacheCtrl.isCacheOff())
//             {
//                 g_api->log(rec->session, LSI_LOG_DEBUG,
//                            "[%s]checkAssignHandler returned, for cache disabled.\n",
//                            ModuleNameStr);
//                 clearHooks(rec->session);
//                 return 0;
//             }
        }
    }

    MyMData *myData = (MyMData *) g_api->get_module_data(rec->session, &MNAME,
                      LSI_DATA_HTTP);
    assert(myData == NULL);

    char host[512] = {0};
    int hostLen = g_api->get_req_var_by_id(rec->session,
                                           LSI_VAR_SERVER_NAME, host, 512);
    char port[12] = {0};
    int portLen = g_api->get_req_var_by_id(rec->session,
                                           LSI_VAR_SERVER_PORT, port, 12);

    //host:port uri
    char *uri = new char[uriLen + hostLen + portLen + 2];
    strncpy(uri, host, hostLen);
    uri[hostLen] = ':';
    strncpy(uri + hostLen + 1, port, portLen);
    g_api->get_req_org_uri(rec->session, uri + hostLen + 1 + portLen,
                           uriLen + 1);
    uriLen += (hostLen + 1 + portLen); //Set the the right uriLen
    uri[uriLen] = 0x00; //NULL terminated

    myData = new MyMData;
    memset(myData, 0, sizeof(MyMData));
    myData->pConfig = pConfig;
    myData->pOrgUri = uri;
    myData->iMethod = method;
    myData->hkptIndex = 0;

    //Set to true but not the below just for not to re-check cache state or
    //re-store it
    //bool doPublic = cacheCtrl.isPublicCacheable() || myData->pConfig->isCheckPublic();
    bool doPublic = true; 
    
    myData->iCacheState = hasCache(rec, myData,
                                   cacheCtrl.getFlags() & CacheCtrl::no_vary,
                                   myData->pOrgUri, uriLen,
                                   myData->pConfig->getStore(), 
                                   &myData->cePublicHash, 
                                   &myData->cePrivateHash,
                                   pConfig, 
                                   &myData->pEntry,
                                   doPublic);
    g_api->set_module_data(rec->session, &MNAME, LSI_DATA_HTTP, (void *)myData);


    if (myData->iMethod == HTTP_PURGE || myData->iMethod == HTTP_REFRESH)
    {
        if(LS_OK != g_api->register_req_handler(rec->session, &MNAME, 0))
        {
            g_api->log(rec->session, LSI_LOG_WARN,
                   "[%s]checkAssignHandler register_req_handler failed.\n", 
                   ModuleNameStr);
            g_api->free_module_data(rec->session, &MNAME, LSI_DATA_HTTP,
                                httpRelease);
        }
        return 0;
    }

    myData->cacheCtrl = cacheCtrl;

    if (myData->iCacheState != CE_STATE_NOCACHE)
    {
        if ((myData->iCacheState == CE_STATE_HAS_RIVATECACHE &&
             (myData->pConfig->isCheckPublic() || myData->pConfig->isPrivateCheck()))
            ||
            (myData->iCacheState == CE_STATE_HAS_PUBLIC_CACHE
             && myData->pConfig->isCheckPublic()))
        {
//             int countCurHitsPercent = myData->pConfig->getBypassPercentage() * myData->pEntry->getTestHits();
//             int countNextHitsPercent = myData->pConfig->getBypassPercentage() + countCurHitsPercent;
//             myData->pEntry->incTestHits();

            if (LS_OK != g_api->register_req_handler(rec->session, &MNAME, 0))
            {
                g_api->log(rec->session, LSI_LOG_WARN,
                       "[%s]checkAssignHandler register_req_handler failed.\n", 
                       ModuleNameStr);
                g_api->free_module_data(rec->session, &MNAME, LSI_DATA_HTTP,
                                    httpRelease);
            }
            else
            {
                //myData->pEntry->incHits();
                myData->iHaveAddedHook = 3; //state of using handler
                g_api->log(rec->session, LSI_LOG_DEBUG,
                       "[%s]checkAssignHandler register_req_handler OK.\n", 
                       ModuleNameStr);
            }
        }
        else
        {
            g_api->log(rec->session, LSI_LOG_INFO,
                   "[%s]checkAssignHandler found cachestate %d but set to not check.\n", 
                   ModuleNameStr, myData->iCacheState);
            g_api->free_module_data(rec->session, &MNAME, LSI_DATA_HTTP,
                                httpRelease);
        }
    }
    else if (myData->iMethod == HTTP_GET &&
             isReqCacheable(rec, pConfig))
    {
        if (!myData->cacheCtrl.isCacheOff())
        {
            //only 'GET' need to store, 'HEAD' won't
            int enableHkpt = LSI_HKPT_RCVD_RESP_HEADER;
            g_api->enable_hook(rec->session, &MNAME, 1, &enableHkpt, 1);
            myData->iHaveAddedHook = 1;

            //g_api->set_session_hook_flag( rec->_session, LSI_HKPT_RCVD_RESP_BODY, &MNAME, 1 );
            g_api->log(rec->session, LSI_LOG_DEBUG,
                       "[%s]checkAssignHandler Add Hooks.\n", ModuleNameStr);
        }
        else
        {
            //Do not free myData because later may have cache-contrl env enable it
            myData->iHaveAddedHook = 0;
        }
    }
    else
    {
        g_api->free_module_data(rec->session, &MNAME, LSI_DATA_HTTP,
                                httpRelease);
        g_api->log(rec->session, LSI_LOG_DEBUG,
                   "[%s]checkAssignHandler won't do anything and quit.\n", ModuleNameStr);
    }

    return 0;
}


static int checkEnv(lsi_param_t *rec)
{
    MyMData *myData = (MyMData *) g_api->get_module_data(rec->session, &MNAME,
                      LSI_DATA_HTTP);
    if(myData == NULL)
        return 0;

    CacheCtrl &cacheCtrl = myData->cacheCtrl;
    if (rec->ptr1 != NULL && rec->len1 > 0)
        cacheCtrl.parse((const char *)rec->ptr1, rec->len1);

    if (cacheCtrl.isCacheOff() && myData->iHaveAddedHook == 1)
    {
        clearHooksOnly(rec->session);
        //myData->iHaveAddedHook = 0;
    }
    else if (!cacheCtrl.isCacheOff() && myData->iHaveAddedHook == 0)
    {
        int hkpt = LSI_HKPT_RCVD_RESP_HEADER;
        g_api->enable_hook(rec->session, &MNAME, 1, &hkpt, 1);
        myData->iHaveAddedHook = 1;
        g_api->log(rec->session, LSI_LOG_DEBUG,
                   "[%s]checkEnv Add Hooks.\n", ModuleNameStr);
    }

    return 0;
}

int releaseIpCounter(void *data)
{
    //No malloc, needn't free, but functions must be presented.
    return 0;
}


static lsi_serverhook_t serverHooks[] =
{
    {LSI_HKPT_RECV_REQ_HEADER, checkAssignHandler, LSI_HOOK_EARLY, LSI_FLAG_ENABLED},
    {LSI_HKPT_HTTP_END, endCache, LSI_HOOK_LAST + 1, LSI_FLAG_ENABLED},
    {LSI_HKPT_HANDLER_RESTART, cancelCache, LSI_HOOK_LAST + 1, LSI_FLAG_ENABLED},  //Disabled

    {LSI_HKPT_RCVD_RESP_HEADER, createEntry, LSI_HOOK_LAST + 1, 0}, //Disabled
    {LSI_HKPT_RCVD_RESP_BODY, cacheTofile, LSI_HOOK_LAST + 1, 0},   //Disabled
    {LSI_HKPT_SEND_RESP_BODY, cacheTofileFilter, LSI_HOOK_LAST, 0},   //Disabled
    LSI_HOOK_END   //Must put this at the end position
};


static int init(lsi_module_t *pModule)
{
    g_api->init_module_data(pModule, httpRelease, LSI_DATA_HTTP);
    g_api->init_module_data(pModule, releaseIpCounter, LSI_DATA_IP);
    g_api->register_env_handler("cache-control", 13, checkEnv);
//     g_api->register_env_handler("setcachedata", 12, setCacheUserData);
//     g_api->register_env_handler("getcachedata", 12, getCacheUserData);

    return 0;
}


int isModified(lsi_session_t *session, CeHeader &CeHeader, char *etag,
               int etagLen)
{
    int len;
    const char *buf = NULL;

    if (etagLen > 0)
    {
        buf = g_api->get_req_header(session, "If-None-Match", 13, &len);
        if (buf && len == etagLen && memcmp(etag, buf, etagLen) == 0)
            return 0;
    }

    buf = g_api->get_req_header(session, "If-Modified-Since", 17, &len);
    if (buf && len >= RFC_1123_TIME_LEN &&
        CeHeader.m_tmLastMod <= DateTime::parseHttpTime(buf))
        return 0;

    return 1;
}


static int handlerProcess(lsi_session_t *session)
{
    MyMData *myData = (MyMData *)g_api->get_module_data(session, &MNAME,
                      LSI_DATA_HTTP);
    if (!myData)
    {
        g_api->log(session, LSI_LOG_ERROR,
                   "[%s]internal error during handlerProcess.\n", ModuleNameStr);
        return 500;
    }

    if (myData->iMethod == HTTP_PURGE || myData->iMethod == HTTP_REFRESH)
    {
        int len;
        const char *pIP = g_api->get_client_ip(session, &len);
        if (g_api->get_client_access(session) != 0 && 
            (!pIP || strncmp(pIP, "127.0.0.1", 9) != 0))
        {
            g_api->free_module_data(session, &MNAME, LSI_DATA_HTTP,
                                    httpRelease);
            return 405;
        }

        if (myData->pEntry)
        {
            if (myData->iMethod == HTTP_PURGE)
            {
                myData->pConfig->getStore()->purge(myData->pEntry);
                g_api->append_resp_body(session, "Purged.", 7);
            }
            else
            {
                myData->pConfig->getStore()->refresh(myData->pEntry);
                g_api->append_resp_body(session, "Refreshed.", 10);
            }
        }
        else
            g_api->append_resp_body(session, "No such entry.", 14);
        
        g_api->end_resp(session);
        g_api->free_module_data(session, &MNAME, LSI_DATA_HTTP,
                                httpRelease);
        return 200;
    }


    char tmBuf[RFC_1123_TIME_LEN + 1];
    int len;
    int fd = myData->pEntry->getFdStore();
    CeHeader &CeHeader = myData->pEntry->getHeader();

    int hitIdx = (myData->iCacheState == CE_STATE_HAS_RIVATECACHE) ? 1 : 0;

    char *buff = NULL;
    char *pBuffOrg = NULL;
    if (myData->pEntry->getPart2Offset() - myData->pEntry->getPart1Offset() >
        0)
    {
#ifdef CACHE_RESP_HEADER
        if (myData->m_pEntry->m_sRespHeader.len() > 0) //has it
            buff = (char *)(myData->m_pEntry->m_sRespHeader.c_str());
        else
#endif
        {
            buff  = (char *)mmap((caddr_t)0, myData->pEntry->getPart2Offset(),
                                 PROT_READ, MAP_SHARED, fd, 0);
            if (buff == (char *)(-1))
            {
                g_api->free_module_data(session, &MNAME, LSI_DATA_HTTP,
                                        httpRelease);
                return 500;
            }
            pBuffOrg = buff;
            buff += myData->pEntry->getPart1Offset();
        }

        if (CeHeader.m_lenETag > 0)
        {
            g_api->set_resp_header(session, LSI_RSPHDR_ETAG, NULL, 0, buff,
                                   CeHeader.m_lenETag, LSI_HEADEROP_SET);
            if (!isModified(session, CeHeader, buff, CeHeader.m_lenETag))
            {
                g_api->set_resp_header(session, LSI_RSPHDR_UNKNOWN, 
                                       s_x_cached, sizeof(s_x_cached) - 1,
                                       s_hits[hitIdx], s_hitsLen[hitIdx],
                                       LSI_HEADEROP_SET);

                g_api->set_status_code(session, 304);
                if (pBuffOrg)
                    munmap((caddr_t)pBuffOrg, myData->pEntry->getPart2Offset());
                g_api->free_module_data(session, &MNAME, LSI_DATA_HTTP,
                                        httpRelease);
                g_api->end_resp(session);
                return 0;
            }
        }

        buff += CeHeader.m_lenETag;
        len = myData->pEntry->getPart2Offset() - myData->pEntry->getPart1Offset() -
              CeHeader.m_lenETag;
        g_api->set_resp_header2(session, buff, len, LSI_HEADEROP_SET);
    }

    if (CeHeader.m_tmLastMod != 0)
    {
        g_api->set_resp_header(session, LSI_RSPHDR_LAST_MODIFIED, NULL, 0,
                               DateTime::getRFCTime(CeHeader.m_tmLastMod, tmBuf),
                               RFC_1123_TIME_LEN, LSI_HEADEROP_SET);
    }
    
    g_api->set_resp_header(session, LSI_RSPHDR_UNKNOWN, 
                           s_x_cached, sizeof(s_x_cached) - 1,
                           s_hits[hitIdx], s_hitsLen[hitIdx],
                           LSI_HEADEROP_SET);

//     time_t tmExpired = DateTime_s_curTime + 604800;
//     g_api->set_resp_header(session, "Expires", 7,
//                                     DateTime::getRFCTime(tmExpired, buf), RFC_1123_TIME_LEN);
//
//     g_api->set_resp_header(session, "Cache-Control", 13, "max-age=604800", 14);


//      CeHeader.m_tmExpire > (int32_t)DateTime_s_curTime
//      unlink file ???

    g_api->set_status_code(session, CeHeader.m_statusCode);

    int ret  = 0;
    if (myData->iMethod == HTTP_GET)
    {
        char filePath[max_file_len] = {0};
        myData->pEntry->getFilePath(filePath, max_file_len);
        
//        g_api->remove_resp_header(session, LSI_RSPHDR_TRANSFER_ENCODING, NULL, 0);
        off_t length = myData->pEntry->getContentTotalLen() -
                       (myData->pEntry->getPart2Offset() - myData->pEntry->getPart1Offset());
        off_t startPos = myData->pEntry->getPart2Offset();

        if (myData->pEntry->isGzipped())
        {
            g_api->set_resp_header(session, LSI_RSPHDR_CONTENT_ENCODING,
                                   NULL, 0, "gzip", 4, LSI_HEADEROP_SET);
            g_api->log(session, LSI_LOG_DEBUG,
                       "[%s]set_resp_header [Content-Encoding: gzip].\n",
                       ModuleNameStr);
            g_api->set_resp_buffer_gzip_flag(session, 1);
        }
        else
            g_api->set_resp_buffer_gzip_flag(session, 0);

        g_api->set_resp_content_length(session, length);
        if (g_api->send_file(session, filePath, startPos, length) == 0)
            g_api->end_resp(session);
        else
            ret = 500;
    }
    else //HEAD
        g_api->end_resp(session);

    if (pBuffOrg)
        munmap((caddr_t)pBuffOrg, myData->pEntry->getPart2Offset());

    g_api->free_module_data(session, &MNAME, LSI_DATA_HTTP,
                            httpRelease);
    return ret;
}

lsi_reqhdlr_t cache_handler = { handlerProcess, NULL, NULL, NULL };
lsi_confparser_t cacheDealConfig = { ParseConfig, FreeConfig, paramArray };
lsi_module_t MNAME = { LSI_MODULE_SIGNATURE, init, &cache_handler,
                       &cacheDealConfig, "cache v1.5", serverHooks, {0}
                     };

