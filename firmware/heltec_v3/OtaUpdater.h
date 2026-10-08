#pragma once
#include <Arduino.h>
#include <Update.h>
#include <WebServer.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include "NodeConfig.h"
#include "LedPatterns.h"
#include "BatteryMonitor.h"
#include "WifiMonitor.h"
#include "DisplayOled.h"

// Public verification key (secp256r1 / NIST P-256), generated per install by tools/sign_firmware.py --init
// (the flash scripts run it). The private signing key stays on the desktop host and is never in firmware or git.
#if __has_include("FwPublicKey.h")
#include "FwPublicKey.h"
#else
#error "No firmware signing key yet: run tools/sign_firmware.py --init (makes your keypair and FwPublicKey.h), then build again"
#endif

class OtaUpdater {
public:
    static OtaUpdater& instance() {
        static OtaUpdater inst;
        return inst;
    }

    void begin(uint16_t webPort = 80) {
        m_webServer = new WebServer(webPort);
        setupWebEndpoints();
        m_webServer->begin();
        m_initialized = true;
    }

    void update() {
        if (m_webServer) {
            m_webServer->handleClient();
        }
    }

    bool verifySignature(const uint8_t* sha256Hash, const uint8_t* sigDer, size_t sigLen) {
        mbedtls_pk_context pk;
        mbedtls_pk_init(&pk);

        int ret = mbedtls_pk_parse_public_key(&pk, (const unsigned char*)FW_PUBLIC_KEY_PEM, strlen(FW_PUBLIC_KEY_PEM) + 1);
        if (ret != 0) {
            Serial.printf("{\"type\":\"error\",\"component\":\"ota\",\"msg\":\"public_key_parse_failed\",\"code\":%d}\n", ret);
            mbedtls_pk_free(&pk);
            return false;
        }

        ret = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, sha256Hash, 32, sigDer, sigLen);
        mbedtls_pk_free(&pk);

        if (ret == 0) {
            Serial.println("{\"type\":\"ota\",\"status\":\"signature_valid\",\"auth\":\"verified\"}");
            return true;
        } else {
            Serial.printf("{\"type\":\"error\",\"component\":\"ota\",\"msg\":\"signature_verification_failed\",\"code\":%d}\n", ret);
            return false;
        }
    }

private:
    OtaUpdater() : m_webServer(nullptr), m_initialized(false), m_sigLen(0),
                   m_binReceived(false), m_sigReceived(false) {
        memset(m_sha256Hash, 0, sizeof(m_sha256Hash));
        memset(m_sigBuf, 0, sizeof(m_sigBuf));
    }

    void setupWebEndpoints();

    WebServer* m_webServer;
    bool m_initialized;
    mbedtls_sha256_context m_shaCtx;
    uint8_t m_sha256Hash[32];
    uint8_t m_sigBuf[128];
    size_t m_sigLen;
    bool m_binReceived;
    bool m_sigReceived;
};
