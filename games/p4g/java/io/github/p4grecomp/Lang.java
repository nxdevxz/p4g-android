package io.github.p4grecomp;

import android.content.Context;

import java.util.Locale;

/** Launcher language. This does not change the game's language. */
final class Lang {
    private static boolean portuguese;

    private Lang() {}

    static void load(Context context) {
        String saved = context.getSharedPreferences("launcher", Context.MODE_PRIVATE)
                .getString("language", "");
        portuguese = saved.isEmpty()
                ? Locale.getDefault().getLanguage().equals("pt")
                : saved.equals("pt");
    }

    static void toggle(Context context) {
        portuguese = !portuguese;
        context.getSharedPreferences("launcher", Context.MODE_PRIVATE).edit()
                .putString("language", portuguese ? "pt" : "en").apply();
    }

    static boolean isPortuguese() { return portuguese; }
    static String device() { return portuguese ? "pt" : "en"; }
    static String t(String portugueseText, String english) { return portuguese ? portugueseText : english; }
    static String f(String portugueseText, String english, Object... args) {
        return String.format(portuguese ? new Locale("pt", "BR") : Locale.ENGLISH,
                portuguese ? portugueseText : english, args);
    }
}

