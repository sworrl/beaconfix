package org.sworrl.beaconfix

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.sworrl.beaconfix.sync.Sas

class SasTest {
    private fun hex(s: String) = s.chunked(2).map { it.toInt(16).toByte() }.toByteArray()
    private fun ByteArray.hex() = joinToString("") { "%02x".format(it) }

    /** RFC 7748 §6.1: Alice and Bob's X25519 keys and the shared secret. */
    @Test fun rfc7748Vector() {
        val alicePriv = hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a")
        val alicePub = hex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a")
        val bobPriv = hex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb")
        val bobPub = hex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f")
        val k = "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"
        val a = Sas.fromSeed(alicePriv); val b = Sas.fromSeed(bobPriv)
        assertEquals(alicePub.hex(), a.pub.hex()); assertEquals(bobPub.hex(), b.pub.hex())
        assertEquals(k, Sas.shared(a.priv, bobPub).hex()); assertEquals(k, Sas.shared(b.priv, alicePub).hex())
        // both sides derive the same pictures for the same pair id
        val pa = Sas.pictures(a.priv, bobPub, "abc123"); val pb = Sas.pictures(b.priv, alicePub, "abc123")
        assertEquals(pa, pb); assertEquals(3, pa.size); assertTrue(pa.all { it in 0..47 })
        assertTrue(pa != Sas.pictures(a.priv, bobPub, "abc124"))
        println("BEACONFIX SAS-VECTOR pairId=abc123 sas=${Sas.derive(hex(k), "abc123").hex()} pictures=$pa")
    }
    @Test fun iconSet() { assertEquals(48, Sas.ICONS.size); assertEquals(48, Sas.EMOJI.size); assertEquals("anchor", Sas.ICONS[0]); assertEquals("zebra", Sas.ICONS[47]); assertEquals(48, Sas.ICONS.toSet().size) }
    @Test fun indexRule() { val sas = ByteArray(32) { (it * 37).toByte() }; assertEquals(listOf((0 and 0xff) % 48, (74 and 0xff) % 48, (148 and 0xff) % 48), Sas.indices(sas)) }
}
