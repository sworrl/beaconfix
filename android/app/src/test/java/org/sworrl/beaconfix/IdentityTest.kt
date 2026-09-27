package org.sworrl.beaconfix

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.sworrl.beaconfix.identity.Crypto
import org.sworrl.beaconfix.identity.DeviceEntry
import org.sworrl.beaconfix.identity.IdentityOps
import org.sworrl.beaconfix.identity.LinkOffer
import org.sworrl.beaconfix.identity.LinkStatement

class IdentityTest {
    /** Fixed-seed vectors: the desktop's `--identity-selftest` must print exactly these. */
    @Test fun selfTestVectors() {
        val t = IdentityOps.selfTest()
        println("BEACONFIX SELFTEST id=${t.id} pub=${t.pubB64} sig=${t.sigB64}")
        // RFC 8032 Ed25519 with seed 32×0x01 (well-known public key)
        assertEquals(26, t.id.length)
        assertTrue(t.id.all { it in "0123456789abcdefghjkmnpqrstvwxyz" })
        // deterministic: the same seed twice gives the same signature
        assertEquals(t.sigB64, IdentityOps.selfTest().sigB64)
        assertTrue(Crypto.verify(Crypto.unb64(t.pubB64), "beaconfix-selftest|v1".toByteArray(), Crypto.unb64(t.sigB64)))
    }

    @Test fun base32Crockford() {
        assertEquals("", Crypto.base32Crockford(ByteArray(0)))
        assertEquals("00", Crypto.base32Crockford(byteArrayOf(0)))
        assertEquals("zzzg", Crypto.base32Crockford(byteArrayOf(-1, -1)))
        assertEquals(26, Crypto.base32Crockford(ByteArray(16) { it.toByte() }).length)
        assertEquals("abcd efgh ij", Crypto.grouped("abcdefghij"))
    }

    @Test fun exportRoundTrip() {
        val seed = Crypto.random(32)
        val rec = IdentityOps.newRecord(seed, "Test person", DeviceEntry("Unit test", "android", "2026-01-01T00:00:00Z"))
        val text = IdentityOps.seal(rec, seed, "correct horse battery staple x")
        assertTrue(text.startsWith("BFID1:"))
        val plain = IdentityOps.open(text, "  Correct  horse-battery STAPLE x ")   // normalised passphrase
        assertEquals(rec.id, plain.record.id); assertEquals(Crypto.b64(seed), plain.seed)
        val bad = runCatching { IdentityOps.open(text, "wrong passphrase") }
        assertTrue(bad.isFailure)
        // the JSON bundle itself is accepted too
        val json = String(Crypto.unb64url(text.removePrefix("BFID1:")))
        assertEquals(rec.id, IdentityOps.open(json, "correct horse battery staple x").record.id)
        assertTrue(json.contains("\"kdf\":{\"name\":\"scrypt\",\"n\":32768,\"r\":8,\"p\":1"))
    }

    @Test fun linkStatements() {
        val sa = Crypto.random(32); val sb = Crypto.random(32)
        val a = IdentityOps.newRecord(sa, "A", DeviceEntry("a", "desktop", "2026-01-01T00:00:00Z"))
        val b = IdentityOps.newRecord(sb, "B", DeviceEntry("b", "android", "2026-01-01T00:00:00Z"))
        val ts = "2026-09-26T00:00:00Z"
        // A shows its payload (unsigned); B builds the statement, signs its side, carries both pubs; A countersigns
        val offer = LinkOffer(id = a.id, name = "A", pub = a.pub, ts = ts)
        assertTrue(IdentityOps.verifyOffer(offer))
        assertFalse(IdentityOps.verifyOffer(offer.copy(pub = b.pub)))
        val half = IdentityOps.completeLink(b.id, sb, a.id, ts, b.pub, a.pub)
        assertFalse(half.complete)
        assertEquals(if (half.a == a.id) a.pub else b.pub, half.pubA)
        val sigA = Crypto.b64(Crypto.sign(sa, half.canon.toByteArray()))
        val full = if (half.a == a.id) half.copy(sigA = sigA) else half.copy(sigB = sigA)
        assertTrue(full.complete)
        val pubA = Crypto.unb64(a.pub); val pubB = Crypto.unb64(b.pub)
        val (pa, pb) = if (full.a == a.id) pubA to pubB else pubB to pubA
        assertTrue(IdentityOps.verifyLink(full, pa, pb))
        assertFalse(IdentityOps.verifyLink(full.copy(ts = "2026-09-27T00:00:00Z"), pa, pb))
        assertEquals("beaconfix-link|v1|" + minOf(a.id, b.id) + "|" + maxOf(a.id, b.id) + "|" + ts, full.canon)
        val linked = IdentityOps.linkedSet(a.copy(links = listOf(full)))
        assertEquals(setOf(a.id, b.id), linked)
        // QR text forms round-trip
        assertEquals(offer, IdentityOps.decodeOffer(IdentityOps.encodeOffer(offer)))
        assertTrue(IdentityOps.encodeOffer(offer).startsWith("BFLNK1:"))
        assertEquals(full, IdentityOps.decodeStatement(IdentityOps.encodeStatement(full)))
        assertTrue(IdentityOps.encodeStatement(full).startsWith("BFLINK1:"))
    }

    @Test fun authCanon() {
        assertEquals("beaconfix-auth|v1|desk|n0nce|abc|Pixel", IdentityOps.authCanon("desk", "n0nce", "abc", "Pixel"))
        val seed = ByteArray(32) { 1 }
        val sig = Crypto.sign(seed, IdentityOps.authCanon("h", "n", "i", "d").toByteArray())
        assertEquals(64, sig.size)
        assertTrue(Crypto.verify(Crypto.publicKey(seed), IdentityOps.authCanon("h", "n", "i", "d").toByteArray(), sig))
    }
}
