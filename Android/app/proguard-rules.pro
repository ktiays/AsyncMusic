# The native engine calls these back by name.
-keep class me.ktiays.asyncmusic.engine.NativeEngine { *; }
-keep interface me.ktiays.asyncmusic.engine.NativeEngine$Callbacks { *; }
-keepclassmembers class * implements me.ktiays.asyncmusic.engine.NativeEngine$Callbacks { *; }
