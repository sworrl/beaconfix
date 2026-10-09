#pragma once
#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <Preferences.h>

// Detections a node makes while it follows a phone that's out of range. They go to the "spiffs" partition (128 KB on
// an ESP32 DevKit, 1.5 MB on a Heltec V3) in two rotating files, oldest dropped first, and are handed to the phone over
// BLE when it's back, each line tagged "sf":<seq> (consecutive), then {"type":"sf_end","first","last"}. The phone
// answers "sf ack <seq>" for the unbroken run it saved, and "sf dump" again for anything missing; until then it stays.
class StoreForward {
public:
    static StoreForward& instance() {
        static StoreForward inst;
        return inst;
    }

    void begin() {
        m_ok = LittleFS.begin(true, "/sf", 4, "spiffs");
        Preferences prefs;
        prefs.begin("sf", true);
        m_nextSeq = prefs.getUInt("seq", 1) + kSeqSaveEvery;   // skip what may have been used since the last save
        prefs.end();
        if (!m_ok) return;
        m_rotateBytes = LittleFS.totalBytes() * 35 / 100;   // two files, each up to ~1/3 of the partition
        m_pending = countLines(kOld) + countLines(kNew);
        // Lines still on flash: carry on right after them, so the phone sees one unbroken run (a gap means "resend")
        const uint32_t last = max(lastSeq(kOld), lastSeq(kNew));
        if (last) m_nextSeq = last + 1;
    }

    bool ok() const { return m_ok; }
    uint32_t pending() const { return m_pending; }
    bool dumping() const { return m_dumping; }

    // One detection line (a JSON object) to keep for the phone
    void store(const char* json) {
        if (!m_ok || !json || json[0] != '{') return;
        File f = LittleFS.open(kNew, FILE_APPEND);
        if (!f) return;
        f.printf("{\"sf\":%lu,%s\n", (unsigned long)m_nextSeq, json + 1);
        const size_t size = f.size();
        f.close();
        m_pending++;
        if (++m_nextSeq % kSeqSaveEvery == 0) saveSeq();
        if (size > m_rotateBytes) {
            m_pending -= countLines(kOld);
            LittleFS.remove(kOld);
            LittleFS.rename(kNew, kOld);
        }
    }

    // The phone subscribed: start handing over what's stored
    void startDump() {
        if (!m_ok || m_pending == 0) return;
        m_dumping = true;
        m_fileIdx = 0;
        m_offset = 0;
        m_sent = 0;
        m_firstSent = 0;
    }

    void stopDump() { m_dumping = false; }

    // Called every loop pass with a function that sends one line (false = not sent, try it again later)
    template <typename SendFn>
    void pump(bool connected, SendFn send) {
        if (!m_dumping) return;
        if (!connected) { m_dumping = false; return; }
        // One line per 25 ms: faster than that the BLE notify buffers overflow and lines are lost on the way
        if (millis() - m_lastSendMs < 25) return;
        for (int n = 0; n < 8; ++n) {
            const char* path = m_fileIdx == 0 ? kOld : kNew;
            File f = LittleFS.open(path, FILE_READ);
            if (!f || m_offset >= f.size()) {
                if (f) f.close();
                if (m_fileIdx == 0) { m_fileIdx = 1; m_offset = 0; continue; }
                char end[96];
                snprintf(end, sizeof(end), "{\"type\":\"sf_end\",\"sent\":%lu,\"first\":%lu,\"last\":%lu}",
                         (unsigned long)m_sent, (unsigned long)m_firstSent, (unsigned long)m_lastSent);
                if (send(end)) m_dumping = false;   // else the end marker again next pass
                m_lastSendMs = millis();
                return;
            }
            f.seek(m_offset);
            String line = f.readStringUntil('\n');
            const size_t next = f.position();
            f.close();
            const uint32_t seq = seqOf(line);
            if (line.length() < 2 || seq <= m_acked) { m_offset = next; continue; }   // skipped: doesn't count against the pace
            if (!send(line.c_str())) break;   // buffers full: the same line again next time, never a gap
            m_offset = next;
            if (!m_firstSent) m_firstSent = seq;
            m_lastSent = seq;
            m_sent++;
            break;
        }
        m_lastSendMs = millis();
    }

    // The phone saved everything up to seq; drop what's covered
    void ack(uint32_t seq) {
        if (seq > m_acked) m_acked = seq;
        if (!m_ok) return;
        if (lastSeq(kOld) <= m_acked) { m_pending -= countLines(kOld); LittleFS.remove(kOld); }
        if (lastSeq(kNew) <= m_acked) { m_pending -= countLines(kNew); LittleFS.remove(kNew); }
        if (!LittleFS.exists(kOld) && !LittleFS.exists(kNew)) m_pending = 0;
    }

    void clear() {
        if (!m_ok) return;
        LittleFS.remove(kOld);
        LittleFS.remove(kNew);
        m_pending = 0;
        m_dumping = false;
    }

    size_t usedBytes() const { return m_ok ? LittleFS.usedBytes() : 0; }
    size_t totalBytes() const { return m_ok ? LittleFS.totalBytes() : 0; }

private:
    StoreForward() = default;

    static constexpr const char* kOld = "/q_old.log";
    static constexpr const char* kNew = "/q_new.log";
    static constexpr uint32_t kSeqSaveEvery = 64;

    static uint32_t seqOf(const String& line) {
        // lines start {"sf":<seq>,
        if (!line.startsWith("{\"sf\":")) return 0;
        return (uint32_t)strtoul(line.c_str() + 6, nullptr, 10);
    }

    uint32_t countLines(const char* path) {
        File f = LittleFS.open(path, FILE_READ);
        if (!f) return 0;
        uint32_t n = 0;
        uint8_t buf[256];
        while (f.available()) {
            const int r = f.read(buf, sizeof(buf));
            for (int i = 0; i < r; ++i) if (buf[i] == '\n') n++;
        }
        f.close();
        return n;
    }

    // Seq of the last line in a file (0 when there's no file)
    uint32_t lastSeq(const char* path) {
        File f = LittleFS.open(path, FILE_READ);
        if (!f) return 0;
        const size_t size = f.size();
        f.seek(size > 400 ? size - 400 : 0);
        uint32_t last = 0;
        while (f.available()) {
            String line = f.readStringUntil('\n');
            const uint32_t s = seqOf(line);
            if (s) last = s;
        }
        f.close();
        return last;
    }

    void saveSeq() {
        Preferences prefs;
        prefs.begin("sf", false);
        prefs.putUInt("seq", m_nextSeq);
        prefs.end();
    }

    bool m_ok = false;
    bool m_dumping = false;
    uint32_t m_nextSeq = 1;
    uint32_t m_acked = 0;
    uint32_t m_pending = 0;
    uint32_t m_sent = 0;
    uint32_t m_firstSent = 0;
    uint32_t m_lastSent = 0;
    int m_fileIdx = 0;
    size_t m_offset = 0;
    unsigned long m_lastSendMs = 0;
    size_t m_rotateBytes = 44 * 1024;
};
