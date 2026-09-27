package org.sworrl.beaconfix.sync

import org.bouncycastle.crypto.agreement.X25519Agreement
import org.bouncycastle.crypto.generators.HKDFBytesGenerator
import org.bouncycastle.crypto.digests.SHA256Digest
import org.bouncycastle.crypto.params.HKDFParameters
import org.bouncycastle.crypto.params.X25519PrivateKeyParameters
import org.bouncycastle.crypto.params.X25519PublicKeyParameters
import java.security.SecureRandom

/**
 * Pairing v2 short-authentication-string: an X25519 exchange per pair request, HKDF-SHA256 with the pair id as info,
 * three pictures from a fixed set of 48 (index_i = SAS[2i] % 48). Both apps implement exactly this.
 */
object Sas {
    val ICONS = listOf("anchor", "apple", "balloon", "bell", "bicycle", "book", "cactus", "camera", "car", "cat", "cloud", "coffee", "compass", "crown", "diamond", "dog", "duck", "feather", "fish", "flag",
        "flower", "fox", "gift", "guitar", "hammer", "hat", "heart", "house", "key", "kite", "leaf", "lightbulb", "moon", "mountain", "mushroom", "owl", "pencil", "pizza", "rocket", "snowflake",
        "star", "sun", "tent", "tree", "umbrella", "whale", "wrench", "zebra")
    val EMOJI = listOf("⚓", "🍎", "🎈", "🔔", "🚲", "📖", "🌵", "📷", "🚗", "🐱", "☁️", "☕", "🧭", "👑", "💎", "🐶", "🦆", "🪶", "🐟", "🚩",
        "🌸", "🦊", "🎁", "🎸", "🔨", "🎩", "❤️", "🏠", "🔑", "🪁", "🍃", "💡", "🌙", "⛰️", "🍄", "🦉", "✏️", "🍕", "🚀", "❄️",
        "⭐", "☀️", "⛺", "🌳", "☂️", "🐳", "🔧", "🦓")

    class Ephemeral(val priv: X25519PrivateKeyParameters) { val pub: ByteArray get() = priv.generatePublicKey().encoded }
    fun ephemeral(): Ephemeral = Ephemeral(X25519PrivateKeyParameters(SecureRandom()))
    fun fromSeed(seed: ByteArray) = Ephemeral(X25519PrivateKeyParameters(seed, 0))

    fun shared(priv: X25519PrivateKeyParameters, theirPub: ByteArray): ByteArray {
        val a = X25519Agreement(); a.init(priv); val out = ByteArray(a.agreementSize); a.calculateAgreement(X25519PublicKeyParameters(theirPub, 0), out, 0); return out
    }
    fun derive(ikm: ByteArray, pairId: String): ByteArray {
        val h = HKDFBytesGenerator(SHA256Digest()); h.init(HKDFParameters(ikm, ByteArray(0), "beaconfix-pair-sas-v1|$pairId".toByteArray()))
        val out = ByteArray(32); h.generateBytes(out, 0, 32); return out
    }
    fun indices(sas: ByteArray): List<Int> = (0..2).map { (sas[2 * it].toInt() and 0xff) % 48 }
    fun pictures(priv: X25519PrivateKeyParameters, theirPub: ByteArray, pairId: String): List<Int> = indices(derive(shared(priv, theirPub), pairId))
}
