package io.github.p4grecomp;

import android.app.Activity;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

/** Shared launcher palette; does not change the game's renderer or touch overlay. */
final class P4gTheme {
    static final int BACKGROUND = Color.rgb(18, 20, 24);
    static final int PANEL = Color.rgb(35, 40, 51);
    static final int YELLOW = Color.rgb(255, 208, 48);
    static final int INK = Color.rgb(20, 20, 20);
    static final int TEXT = Color.rgb(236, 238, 242);
    static final int MUTED = Color.rgb(185, 194, 211);

    private P4gTheme() {}

    static int dp(android.content.Context context, int value) {
        return Math.round(value * context.getResources().getDisplayMetrics().density);
    }

    static void apply(Activity activity) {
        activity.getWindow().setStatusBarColor(BACKGROUND);
        activity.getWindow().setNavigationBarColor(BACKGROUND);
    }

    static void button(Button button, boolean primary) {
        button.setAllCaps(false);
        button.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        button.setTextSize(14);
        button.setStateListAnimator(null);
        button.setMinHeight(dp(button.getContext(), 48));
        button.setPadding(dp(button.getContext(), 14), dp(button.getContext(), 10),
                dp(button.getContext(), 14), dp(button.getContext(), 10));
        button.setBackgroundTintList(null);
        GradientDrawable shape = new GradientDrawable();
        shape.setColor(primary ? YELLOW : PANEL);
        shape.setCornerRadius(dp(button.getContext(), 16));
        button.setBackground(new RippleDrawable(ColorStateList.valueOf(0x33FFFFFF), shape, null));
        button.setTextColor(new ColorStateList(
                new int[][] {new int[] {-android.R.attr.state_enabled}, new int[] {}},
                new int[] {MUTED, primary ? INK : TEXT}));
    }

    static void section(LinearLayout root, String text) {
        TextView heading = new TextView(root.getContext());
        heading.setText(text);
        heading.setTextColor(MUTED);
        heading.setTextSize(13);
        heading.setTypeface(Typeface.DEFAULT_BOLD);
        heading.setLetterSpacing(0.08f);
        heading.setPadding(0, dp(root.getContext(), 20), 0, dp(root.getContext(), 8));
        root.addView(heading);
    }
}
