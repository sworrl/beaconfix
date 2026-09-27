-keepattributes Signature, InnerClasses, EnclosingMethod, *Annotation*
# Retrofit interfaces
-keepclasseswithmembers,allowshrinking,allowobfuscation interface * { @retrofit2.http.* <methods>; }
-dontwarn org.codehaus.mojo.animal_sniffer.*
-dontwarn okhttp3.internal.platform.**
-dontwarn org.conscrypt.**
-dontwarn org.bouncycastle.**
-dontwarn org.openjsse.**
# kotlinx.serialization
-keepclassmembers class kotlinx.serialization.json.** { *** Companion; }
-keepclasseswithmembers class kotlinx.serialization.json.** { kotlinx.serialization.KSerializer serializer(...); }
-keep,includedescriptorclasses class org.sworrl.beaconfix.**$$serializer { *; }
-keepclassmembers class org.sworrl.beaconfix.** { *** Companion; }
-keepclasseswithmembers class org.sworrl.beaconfix.** { kotlinx.serialization.KSerializer serializer(...); }
# osmdroid
-dontwarn org.osmdroid.**
# Glance action callbacks and widget classes are looked up by name
-keep class * extends androidx.glance.appwidget.action.ActionCallback { *; }
-keep class * extends androidx.glance.appwidget.GlanceAppWidgetReceiver { *; }
-keep class * extends androidx.glance.appwidget.GlanceAppWidget { *; }
# BouncyCastle lightweight API (no provider registration)
-dontwarn org.bouncycastle.**
-keep class org.bouncycastle.crypto.** { *; }
# ZXing
-dontwarn com.google.zxing.**
