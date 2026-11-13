package io.github.p4grecomp;

import android.app.AlertDialog;
import android.content.res.ColorStateList;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.IOException;
import java.util.List;

/** Persistent driver selection. File operations run in the launcher, outside the game process. */
final class DriverSelector {
    private final Launcher activity;
    private AlertDialog dialog;
    private boolean busy;

    private interface Operation { void run() throws IOException; }

    DriverSelector(Launcher activity) { this.activity = activity; }
    private String t(String portuguese, String english) { return Lang.t(portuguese, english); }

    boolean busy() { return busy; }

    void dismiss() { if (dialog != null) dialog.dismiss(); }

    void show() {
        if (busy || (dialog != null && dialog.isShowing())) return;
        reload(null);
    }

    private void reload(Operation operation) {
        busy = true;
        dismiss();
        AlertDialog progress = new AlertDialog.Builder(activity).setTitle(t("Driver gráfico", "GPU driver"))
                .setMessage(t("Atualizando lista de drivers…", "Updating driver list...")).setCancelable(false).create();
        dialog = progress;
        progress.show();
        new Thread(() -> {
            List<CustomDrivers.Driver> drivers = null;
            String selected = "", error = null;
            try {
                if (operation != null) operation.run();
                drivers = CustomDrivers.list(activity);
                selected = CustomDrivers.active(activity);
            } catch (IOException | RuntimeException e) {
                error = e.getMessage();
                if (error == null) error = e.toString();
            }
            final List<CustomDrivers.Driver> saved = drivers;
            final String active = selected, failure = error;
            activity.runOnUiThread(() -> {
                busy = false;
                if (activity.isFinishing() || activity.isDestroyed()) return;
                progress.dismiss();
                if (failure == null) showList(saved, active);
                else dialog = new AlertDialog.Builder(activity).setTitle(t("Driver gráfico", "GPU driver"))
                        .setMessage(failure).setPositiveButton(t("Voltar", "Back"), (d, w) -> reload(null))
                        .setNegativeButton(t("Fechar", "Close"), null).show();
            });
        }, "P4G-driver-list").start();
    }

    private void showList(List<CustomDrivers.Driver> drivers, String active) {
        LinearLayout content = new LinearLayout(activity);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setBackgroundColor(P4gTheme.BACKGROUND);
        int padding = P4gTheme.dp(activity, 12);
        content.setPadding(padding, padding, padding, padding);
        TextView hint = new TextView(activity);
        String last = CustomDrivers.lastRun(activity);
        hint.setText(t("A seleção será aplicada ao abrir o jogo novamente.",
                "Selection applies the next time you open the game.")
                + (last.isEmpty() ? "" : t("\nÚltima inicialização: ", "\nLast initialization: ") + last));
        hint.setTextColor(P4gTheme.MUTED);
        hint.setPadding(0, 0, 0, padding);
        content.addView(hint);
        LinearLayout rows = new LinearLayout(activity);
        rows.setOrientation(LinearLayout.VERTICAL);
        addDriver(rows, null, active);
        for (CustomDrivers.Driver driver : drivers) addDriver(rows, driver, active);
        ScrollView scroll = new ScrollView(activity);
        scroll.addView(rows);
        int screenHeight = activity.getResources().getDisplayMetrics().heightPixels;
        int height = Math.max(P4gTheme.dp(activity, 100), screenHeight - P4gTheme.dp(activity, 210));
        content.addView(scroll, new LinearLayout.LayoutParams(-1, Math.min(height, P4gTheme.dp(activity, 300))));
        dialog = new AlertDialog.Builder(activity).setTitle(t("Driver gráfico", "GPU driver")).setView(content)
                .setPositiveButton(t("Importar", "Import"), (d, w) -> activity.pick(Launcher.PICK_DRIVER, "*/*", false))
                .setNegativeButton(t("Fechar", "Close"), null).show();
    }

    private void addDriver(LinearLayout rows, CustomDrivers.Driver driver, String active) {
        LinearLayout row = new LinearLayout(activity);
        row.setGravity(Gravity.CENTER_VERTICAL);
        RadioButton choice = new RadioButton(activity);
        choice.setText(driver == null ? t("Driver do sistema", "System driver")
                : driver.name + (driver.version.isEmpty() ? "" : "\n" + driver.version));
        choice.setTextSize(16);
        choice.setTextColor(P4gTheme.TEXT);
        choice.setButtonTintList(ColorStateList.valueOf(P4gTheme.YELLOW));
        choice.setChecked(driver == null ? active.isEmpty() : active.equals(driver.id));
        choice.setMinHeight(P4gTheme.dp(activity, 56));
        choice.setOnClickListener(v -> reload(() -> {
            if (driver == null) CustomDrivers.useSystem(activity);
            else CustomDrivers.select(activity, driver);
        }));
        row.addView(choice, new LinearLayout.LayoutParams(0, -2, 1));
        if (driver != null) {
            Button remove = new Button(activity);
            P4gTheme.button(remove, false);
            remove.setText(t("Excluir", "Delete"));
            remove.setContentDescription(t("Excluir ", "Delete ") + driver.name);
            remove.setOnClickListener(v -> reload(() -> CustomDrivers.delete(activity, driver)));
            row.addView(remove);
        }
        rows.addView(row);
    }
}

