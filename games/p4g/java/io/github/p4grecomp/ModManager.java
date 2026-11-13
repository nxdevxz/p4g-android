package io.github.p4grecomp;

import android.app.AlertDialog;
import java.io.IOException;
import java.util.List;
import java.util.Map;

/** Scans and changes mod folders off the UI thread. Changes take effect on the next game start. */
final class ModManager {
    private final Launcher activity;
    private AlertDialog dialog;
    private boolean busy;

    ModManager(Launcher activity) { this.activity = activity; }
    private String t(String portuguese, String english) { return Lang.t(portuguese, english); }

    boolean busy() { return busy; }

    void dismiss() { if (dialog != null) dialog.dismiss(); }

    private boolean alive() { return !activity.isFinishing() && !activity.isDestroyed(); }

    void show() {
        if (busy || (dialog != null && dialog.isShowing())) return;
        reload(null, false);
    }

    private void reload(ModFiles.Mod change, boolean enabled) {
        busy = true;
        AlertDialog progress = new AlertDialog.Builder(activity)
                .setTitle(t("Mods e traduções", "Mods and translations"))
                .setMessage(change == null ? t("Lendo mods instalados…", "Reading installed mods...")
                        : t("Atualizando mod…", "Updating mod..."))
                .setCancelable(false).create();
        dialog = progress;
        progress.show();
        new Thread(() -> {
            ModFiles.Snapshot snapshot = null;
            String error = null;
            try {
                if (change != null) ModFiles.setEnabled(change, enabled);
                snapshot = ModFiles.scan(Launcher.dataDir());
            } catch (IOException | RuntimeException e) {
                error = e.getMessage();
                if (error == null) error = e.toString();
            }
            final ModFiles.Snapshot result = snapshot;
            final String failure = error;
            activity.runOnUiThread(() -> {
                busy = false;
                if (!alive()) return;
                progress.dismiss();
                if (failure == null) showMods(result);
                else dialog = new AlertDialog.Builder(activity).setTitle(t("Não foi possível atualizar os mods", "Could not read or update mods"))
                        .setMessage(failure).setPositiveButton(t("Tentar novamente", "Retry"), (d, w) -> reload(null, false))
                        .setNegativeButton(t("Fechar", "Close"), null).show();
            });
        }, "P4G-mod-manager").start();
    }

    private void showMods(ModFiles.Snapshot snapshot) {
        AlertDialog.Builder builder = new AlertDialog.Builder(activity).setTitle(t("Mods e traduções", "Mods and translations"))
                .setNegativeButton(t("Fechar", "Close"), null);
        if (snapshot.mods.isEmpty()) {
            builder.setMessage(t("Nenhum mod encontrado. Importe uma tradução com Importar CPK ou coloque um mod em "
                    + "mods/<nome>/romfs/. As mudanças serão aplicadas ao abrir o jogo novamente.",
                    "No mods found. Import a translation with Import CPK, or place a mod in "
                    + "mods/<name>/romfs/. Changes apply the next time you open the game."));
        } else {
            String[] rows = new String[snapshot.mods.size()];
            for (int i = 0; i < rows.length; ++i) {
                ModFiles.Mod mod = snapshot.mods.get(i);
                int conflicts = snapshot.conflictCount(mod);
                rows[i] = mod.name + "\n" + (mod.enabled ? t("Ativado", "Enabled") : t("Desativado", "Disabled"))
                        + " · " + mod.files.size() + t(" arquivos", " files")
                        + (conflicts == 0 ? "" : " · " + conflicts + t(" arquivos em conflito", " conflicting files"));
            }
            builder.setItems(rows, (d, index) -> showMod(snapshot, snapshot.mods.get(index)));
        }
        if (!snapshot.conflicts.isEmpty())
            builder.setNeutralButton(t("Conflitos (", "Conflicts (") + snapshot.conflicts.size() + ")", (d, w) -> showConflicts(snapshot));
        dialog = builder.show();
    }

    private void showMod(ModFiles.Snapshot snapshot, ModFiles.Mod mod) {
        String path = Launcher.dataDir().toPath().relativize(mod.path.toPath()).toString();
        dialog = new AlertDialog.Builder(activity).setTitle(mod.name)
                .setMessage((mod.enabled ? t("Ativado", "Enabled") : t("Desativado", "Disabled")) + " · "
                        + mod.files.size() + t(" arquivos\n", " files\n") + path
                        + t("\n\nAs mudanças serão aplicadas ao abrir o jogo novamente.",
                                "\n\nChanges apply the next time you open the game."))
                .setPositiveButton(mod.enabled ? t("Desativar", "Disable") : t("Ativar", "Enable"), (d, w) -> toggle(snapshot, mod))
                .setNeutralButton(t("Ver arquivos", "View files"), (d, w) -> showFiles(snapshot, mod))
                .setNegativeButton(t("Voltar", "Back"), (d, w) -> showMods(snapshot)).show();
    }

    private void toggle(ModFiles.Snapshot snapshot, ModFiles.Mod mod) {
        int overlaps = mod.enabled ? 0 : snapshot.overlapsWhenEnabled(mod);
        if (overlaps == 0) {
            reload(mod, !mod.enabled);
            return;
        }
        dialog = new AlertDialog.Builder(activity).setTitle(t("Ativar mod com conflitos?", "Enable overlapping mod?"))
                .setMessage(t("Este mod substitui ", "This mod replaces ") + overlaps
                        + t(" arquivos também fornecidos por mods ativos. A última pasta em ordem alfabética terá prioridade.",
                                " files also supplied by enabled mods. For each file, the last folder in alphabetical order takes priority."))
                .setPositiveButton(t("Ativar", "Enable"), (d, w) -> reload(mod, true))
                .setNegativeButton(t("Voltar", "Back"), (d, w) -> showMod(snapshot, mod)).show();
    }

    private void showFiles(ModFiles.Snapshot snapshot, ModFiles.Mod mod) {
        AlertDialog.Builder builder = new AlertDialog.Builder(activity).setTitle(mod.name + t(" · Arquivos", " · Files"))
                .setNegativeButton(t("Voltar", "Back"), (d, w) -> showMod(snapshot, mod));
        if (mod.files.isEmpty()) builder.setMessage(t("A pasta deste mod está vazia.", "This mod folder is empty."));
        else {
            String[] rows = new String[mod.files.size()];
            for (int i = 0; i < rows.length; ++i) {
                String file = mod.files.get(i);
                List<ModFiles.Mod> owners = snapshot.conflicts.get(ModFiles.key(file));
                rows[i] = file;
                if (mod.enabled && owners != null)
                    rows[i] += t("\nConflito · Usado de ", "\nConflict · Used from ") + owners.get(owners.size() - 1).name;
            }
            builder.setItems(rows, null);
        }
        dialog = builder.show();
    }

    private void showConflicts(ModFiles.Snapshot snapshot) {
        String[] rows = new String[snapshot.conflicts.size()];
        int index = 0;
        for (Map.Entry<String, List<ModFiles.Mod>> entry : snapshot.conflicts.entrySet()) {
            StringBuilder line = new StringBuilder(entry.getKey());
            for (ModFiles.Mod mod : entry.getValue()) line.append("\n").append(mod.name);
            List<ModFiles.Mod> owners = entry.getValue();
            line.append(t("\nUsado de ", "\nUsed from ")).append(owners.get(owners.size() - 1).name);
            rows[index++] = line.toString();
        }
        dialog = new AlertDialog.Builder(activity).setTitle(t("Arquivos em conflito", "Conflicting files"))
                .setItems(rows, null).setNegativeButton(t("Voltar", "Back"), (d, w) -> showMods(snapshot)).show();
    }
}

