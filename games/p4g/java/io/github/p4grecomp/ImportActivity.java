package io.github.p4grecomp;

import android.app.Activity;
import android.app.AlertDialog;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.graphics.Typeface;
import android.view.WindowManager;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.widget.Toast;
import android.widget.Button;
import java.util.Locale;
import java.io.File;
import java.io.IOException;

/** Preparation only: completion returns directly to the launcher. */
public class ImportActivity extends Activity {
    private static final java.util.concurrent.atomic.AtomicBoolean ACTIVE = new java.util.concurrent.atomic.AtomicBoolean();
    static final String EXTRA_URI = "uri", EXTRA_NAME = "name", EXTRA_UPDATE_URI = "update_uri",
            EXTRA_UPDATE_NAME = "update_name";
    TextView status;
    ProgressBar bar;
    volatile boolean running;
    volatile boolean cancelRequested;
    boolean publishing;
    final Object commitLock = new Object();
    final Handler ui = new Handler(Looper.getMainLooper());
    int dp(int v) { return Math.round(v * getResources().getDisplayMetrics().density); }
    String t(String portuguese, String english) { return Lang.t(portuguese, english); }

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        Lang.load(this);
        P4gTheme.apply(this);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(P4gTheme.BACKGROUND);
        root.setPadding(dp(32), dp(32), dp(32), dp(32));
        TextView title = new TextView(this);
        title.setText(t("Preparando P4G", "Preparing P4G"));
        title.setTextColor(P4gTheme.YELLOW);
        title.setTextSize(26);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        root.addView(title);
        bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        bar.setProgressTintList(android.content.res.ColorStateList.valueOf(P4gTheme.YELLOW));
        root.addView(bar, new LinearLayout.LayoutParams(-1, dp(24)));
        status = new TextView(this);
        status.setTextColor(android.graphics.Color.WHITE);
        status.setTextSize(16);
        status.setPadding(0, dp(16), 0, 0);
        root.addView(status);
        Button cancel = new Button(this);
        cancel.setText(t("Cancelar preparação", "Cancel preparation"));
        cancel.setOnClickListener(v -> onBackPressed());
        root.addView(cancel);
        setContentView(root);
        start();
    }

    @Override public void onBackPressed() {
        if (!running) { setResult(RESULT_CANCELED); finish(); }
        else new AlertDialog.Builder(this).setTitle(t("Cancelar preparação?", "Cancel preparation?"))
            .setMessage(t("A instalação temporária será descartada. A instalação anterior continuará disponível.",
                    "The temporary installation will be discarded. Any previous installation stays available."))
            .setPositiveButton(t("Cancelar preparação", "Cancel preparation"), (d, w) -> {
                synchronized (commitLock) {
                    if (publishing) return;
                    cancelRequested = true;
                    Importer.cancel();
                }
                status.setText(t("Cancelando… Aguarde a operação atual terminar.",
                        "Cancelling... Waiting for the current operation to finish."));
            }).setNegativeButton(t("Continuar preparando", "Keep preparing"), null).show();
    }
    void fail(String message) {
        status.setText(message);
        new AlertDialog.Builder(this).setTitle(t("Falha na preparação", "Preparation failed")).setMessage(message)
            .setPositiveButton("OK", (d, w) -> { setResult(RESULT_CANCELED); finish(); })
            .setOnCancelListener(d -> { setResult(RESULT_CANCELED); finish(); }).show();
    }
    @Override protected void onDestroy() {
        synchronized (commitLock) {
            if (running && !publishing) { cancelRequested = true; Importer.cancel(); }
        }
        ui.removeCallbacksAndMessages(null);
        super.onDestroy();
    }
    void start() {
        String uriString =
            getIntent().getStringExtra(EXTRA_URI);

        if (uriString == null || uriString.isEmpty()) {
            status.setText(
                "No game file selected."
            );
            fail("No game file selected.");
            return;
        }

        final Uri uri = Uri.parse(uriString);
        final String updateUriString = getIntent().getStringExtra(EXTRA_UPDATE_URI);
        final Uri updateUri = updateUriString == null || updateUriString.isEmpty() ? null : Uri.parse(updateUriString);

        final String name =
            getIntent().getStringExtra(EXTRA_NAME);
        final String updateName = getIntent().getStringExtra(EXTRA_UPDATE_NAME);

        final File keys =
            Launcher.keysFile(this, "prod.keys");

        final File titleKeys =
            Launcher.keysFile(this, "title.keys");

        final File data;
        try { data = Launcher.installationDir(this); }
        catch (Exception e) { fail(String.valueOf(e.getMessage())); return; }

        final File work =
            new File(getCacheDir(), "import");

        if (!ACTIVE.compareAndSet(false, true)) {
            fail("Another preparation is still finishing. Wait and try again.");
            return;
        }
        running = true;
        Importer.reset();


        status.setText(updateUri == null
                ? "Installing base game data... Keep this screen open and allow enough free space."
                : "Applying the game update over the base game... Keep this screen open and allow enough free space.");

        new Thread(new Runnable() {
            @Override
            public void run() {
                int rc;
                String error = "";
                File staged = null;

                try (
                    ParcelFileDescriptor pfd =
                        getContentResolver()
                            .openFileDescriptor(uri, "r");
                    ParcelFileDescriptor updatePfd = updateUri == null ? null :
                        getContentResolver().openFileDescriptor(updateUri, "r")
                ) {
                    if (pfd == null) {
                        throw new IOException(
                            "Could not open the NSP/XCI"
                        );
                    }
                    staged = InstalledGameFiles.begin(data);

                    int jobs = Math.max(
                        2,
                        Runtime.getRuntime()
                            .availableProcessors()
                    );

                    rc = Importer.run(
                        pfd.getFd(),
                        name == null ? "" : name,
                        updatePfd == null ? -1 : updatePfd.getFd(),
                        updateName == null ? "" : updateName,
                        keys.getPath(),
                        titleKeys.exists()
                            ? titleKeys.getPath()
                            : "",
                        staged.getPath(),
                        work.getPath(),
                        jobs
                    );

                    if (rc != 0) {
                        error = Importer.error();
                    } else {
                        InstalledGameFiles.finish(staged);
                        synchronized (commitLock) {
                            if (cancelRequested) throw new IOException("Preparation cancelled");
                            publishing = true;
                        }
                        InstalledGameFiles.publish(data, staged);
                        staged = null;
                    }

                } catch (Exception e) {
                    rc = 1;
                    error = String.valueOf(
                        e.getMessage()
                    );
                } finally {
                    if (staged != null) {
                        try { InstalledGameFiles.discard(data, staged); }
                        catch (IOException ignored) { }
                    }
                    running = false;
                    ACTIVE.set(false);
                }

                final int result = rc;
                final String message = error;

                ui.post(new Runnable() {
                    @Override
                    public void run() {
                        running = false;


                        if (isFinishing() || isDestroyed()) return;
                        if (cancelRequested) { setResult(RESULT_CANCELED); finish(); return; }
                        if (result == 0) {
                            bar.setProgress(1000);
                            String complete = updateUri == null ? "Game installed." : "Game update installed.";
                            status.setText(complete + " The NSP/XCI files are no longer required to play.");
                            Toast.makeText(ImportActivity.this, complete, Toast.LENGTH_LONG).show();
                            setResult(RESULT_OK);
                            finish();
                        } else {
                            fail("Preparation incomplete:\n\n" + message);
                        }
                    }
                });
            }
        }).start();

        ui.postDelayed(new Runnable() {
            @Override
            public void run() {
                if (!running) {
                    return;
                }
                if (cancelRequested) { ui.postDelayed(this, 500); return; }

                int[] p = Importer.progress();

                int phase = p[0];
                int done = p[1];
                int total = Math.max(1, p[2]);

                double share =
                    (double) Math.min(done, total) / total;

                int value;

                if (phase == 7) {
                    value = 710;
                } else if (phase == 6) {
                    value = 50 + (int) (650 * share);
                } else if (phase <= 1) {
                    value = 20;
                } else if (phase == 2) {
                    value = 700 + (int) (50 * share);
                } else if (phase == 3) {
                    value = 750 + (int) (230 * share);
                } else {
                    value = 990;
                }

                bar.setProgress(value);

                if (phase == 7) {
                    status.setText("Checking the local shader compiler...");
                } else if (phase == 6) {
                    long[] bytes = Importer.bytes();
                    String sizes = bytes[1] > 0 ? String.format(Locale.US,
                            "\nRomFS: %.2f / %.2f GB\nRemaining RomFS: %.2f GB; free: %.2f GB\nAdditional space is needed for modules and shaders.",
                            bytes[0] / 1e9, bytes[1] / 1e9,
                            Math.max(0L, bytes[1] - bytes[0]) / 1e9, data.getUsableSpace() / 1e9) : "";
                    status.setText("Extracting and checking game data (" + (int) (100 * share) + "%)..." + sizes);
                } else if (phase == 2) {
                    status.setText(
                        "Scanning game modules (" +
                        done + " of " + total + ")..."
                    );
                } else if (phase == 3) {
                    status.setText(
                        "Generating local shaders (module " +
                        done + " of " + total + ")..."
                    );
                } else if (phase == 4) {
                    status.setText("Finishing...");
                }

                ui.postDelayed(this, 500);
            }
        }, 500);
    }
}

