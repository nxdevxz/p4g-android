package io.github.p4grecomp;

import android.os.Handler;
import android.os.HandlerThread;
import android.os.Build;
import android.os.SystemClock;
import android.system.Os;
import android.system.OsConstants;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.PorterDuff;
import android.graphics.Rect;
import android.util.DisplayMetrics;
import android.view.View;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.ViewGroup;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.util.Locale;
import java.util.Map;

/** One sample per second, off the render/UI threads; no metric logs or overlay permission. */
final class PerformanceOverlay {
    private final GameActivity activity;
    private final boolean fps, cpu, gpu, ram, frameTime;
    private float[] frameTimes = new float[0];
    private HandlerThread thread;
    private Handler worker;
    private SurfaceView panel;
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint background = new Paint();
    private float baseTextSize;
    private volatile boolean surfaceReady;
    private volatile int generation;
    private int fixedBufferWidth, fixedBufferHeight;
    private long lastTime, lastCpu, lastFrames;
    private final int cpuCores = Math.max(1, Runtime.getRuntime().availableProcessors());
    private String gpuCounter;
    private boolean gpuUnavailable;

    PerformanceOverlay(GameActivity activity, Map<String, String> config) {
        this.activity = activity;
        fps = "1".equals(config.get("NX_SHOW_FPS"));
        cpu = "1".equals(config.get("NX_OVERLAY_CPU"));
        gpu = "1".equals(config.get("NX_OVERLAY_GPU"));
        ram = "1".equals(config.get("NX_OVERLAY_RAM"));
        frameTime = "1".equals(config.get("NX_OVERLAY_FRAME_TIME"));
        if (!(fps || cpu || gpu || ram || frameTime)) return;
        panel = new SurfaceView(activity);
        // NativeActivity gives its normal window surface directly to Vulkan. A TextView in that
        // window can therefore be rendered behind the game. This transparent SurfaceView is a
        // separate Android compositor layer and stays above the Vulkan surface.
        panel.setZOrderOnTop(true);
        panel.getHolder().setFormat(PixelFormat.TRANSLUCENT);
        panel.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override public void surfaceCreated(SurfaceHolder holder) {
                surfaceReady = true;
                requestPhysicalBuffer();
            }
            @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                surfaceReady = true;
                requestPhysicalBuffer();
            }
            @Override public void surfaceDestroyed(SurfaceHolder holder) { surfaceReady = false; }
        });
        panel.addOnLayoutChangeListener((view, left, top, right, bottom,
                oldLeft, oldTop, oldRight, oldBottom) -> requestPhysicalBuffer());
        panel.setClickable(false);
        panel.setFocusable(false);
        panel.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        paint.setTypeface(android.graphics.Typeface.MONOSPACE);
        baseTextSize = P4gTheme.dp(activity, 10);
        paint.setTextSize(baseTextSize);
        paint.setColor(P4gTheme.YELLOW);
        background.setColor(0xB3141414);
        activity.addContentView(panel, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        requestPhysicalBuffer();
    }

    /** Keeps this compositor layer at window pixels even when Vulkan selects a 360p/480p/720p buffer. */
    private void requestPhysicalBuffer() {
        if (panel == null) return;
        panel.post(() -> {
            if (panel == null) return;
            int width = 0, height = 0;
            if (Build.VERSION.SDK_INT >= 30) {
                Rect bounds = Api30.windowBounds(activity);
                width = bounds.width();
                height = bounds.height();
            } else {
                DisplayMetrics metrics = new DisplayMetrics();
                activity.getWindowManager().getDefaultDisplay().getRealMetrics(metrics);
                width = metrics.widthPixels;
                height = metrics.heightPixels;
            }
            // A configuration callback may briefly report the previous orientation.
            int viewWidth = panel.getWidth(), viewHeight = panel.getHeight();
            if (viewWidth > 0 && viewHeight > 0 && (viewWidth > viewHeight) != (width > height)) {
                int swap = width;
                width = height;
                height = swap;
            }
            if (width <= 0 || height <= 0 ||
                    (width == fixedBufferWidth && height == fixedBufferHeight)) return;
            fixedBufferWidth = width;
            fixedBufferHeight = height;
            panel.getHolder().setFixedSize(width, height);
        });
    }

    /** Keeps API 30 references out of verification on older Android releases. */
    private static final class Api30 {
        static Rect windowBounds(GameActivity activity) {
            return activity.getWindowManager().getCurrentWindowMetrics().getBounds();
        }
    }

    void start() {
        if (panel == null || thread != null) return;
        final int token = ++generation;
        thread = new HandlerThread("P4G-overlay", android.os.Process.THREAD_PRIORITY_BACKGROUND);
        thread.start();
        worker = new Handler(thread.getLooper());
        final Handler sampler = worker;
        panel.setVisibility(View.VISIBLE);
        worker.post(new Runnable() {
            private boolean first = true;
            @Override public void run() {
                if (token != generation) return;
                long now = SystemClock.elapsedRealtime();
                long used = cpu ? android.os.Process.getElapsedCpuTime() : 0;
                long frames = -1;
                if (fps) try { frames = GameActivity.nativeFrames(); } catch (UnsatisfiedLinkError ignored) { }
                long elapsed = now - lastTime;
                StringBuilder text = new StringBuilder();
                if (fps) append(text, first || elapsed <= 0 || frames < 0 || lastFrames < 0 ? "FPS: —" :
                        String.format(Locale.US, "FPS: %.1f", Math.max(0L, frames - lastFrames) * 1000.0 / elapsed));
                if (cpu) append(text, first || elapsed <= 0 ? "CPU app: —" :
                        String.format(Locale.US, "CPU app: %.0f%%", Math.min(100.0,
                                Math.max(0L, used - lastCpu) * 100.0 / (elapsed * cpuCores))));
                if (gpu) append(text, "GPU: " + gpuUsage());
                if (ram) append(text, "RAM app: " + residentMemory());
                if (frameTime) {
                    try { frameTimes = GameActivity.nativeFrameTimes(); }
                    catch (UnsatisfiedLinkError ignored) { frameTimes = new float[0]; }
                    float worst = 0;
                    for (float ms : frameTimes) worst = Math.max(worst, ms);
                    append(text, frameTimes.length == 0 ? "Frame: —" : String.format(Locale.US,
                            "Frame: %.1f ms | max %.1f", frameTimes[frameTimes.length - 1], worst));
                }
                lastTime = now; lastCpu = used; lastFrames = frames; first = false;
                String label = text.toString();
                draw(label, token);
                if (token == generation) sampler.postDelayed(this, 1000);
            }
        });
    }

    private void draw(String text, int token) {
        if (token != generation || !surfaceReady || panel == null) return;
        SurfaceHolder holder = panel.getHolder();
        Canvas canvas = null;
        try {
            canvas = holder.lockCanvas();
            if (canvas == null) return;
            canvas.drawColor(Color.TRANSPARENT, PorterDuff.Mode.CLEAR);
            String[] lines = text.split("\\n");
            float shortSide = Math.max(1.0f, Math.min(canvas.getWidth(), canvas.getHeight()));
            float edge = Math.min(P4gTheme.dp(activity, 6), shortSide * 0.02f);
            float pad = Math.min(P4gTheme.dp(activity, 5), shortSide * 0.018f);
            // requestPhysicalBuffer keeps this canvas in window pixels. The fit below remains a
            // safety net for unusual multi-window bounds and long localized metric strings.
            paint.setTextSize(baseTextSize);
            float width = 0;
            for (String line : lines) width = Math.max(width, paint.measureText(line));
            float lineHeight = paint.getTextSize() * 1.20f;
            float widthScale = (canvas.getWidth() - edge * 2.0f - pad * 2.0f) / Math.max(1.0f, width);
            float heightScale = (canvas.getHeight() - edge * 2.0f - pad * 1.4f) /
                    Math.max(1.0f, lines.length * lineHeight);
            float fit = Math.min(1.0f, Math.min(widthScale, heightScale));
            if (fit < 1.0f) {
                paint.setTextSize(Math.max(6.0f, paint.getTextSize() * fit));
                width = 0;
                for (String line : lines) width = Math.max(width, paint.measureText(line));
                lineHeight = paint.getTextSize() * 1.20f;
            }
            float panelWidth = Math.min(canvas.getWidth() - edge * 2.0f, width + pad * 2.0f);
            float graphHeight = frameTime ? Math.min(P4gTheme.dp(activity, 48), shortSide * 0.15f) : 0;
            if (frameTime) panelWidth = Math.min(canvas.getWidth() - edge * 2.0f,
                    Math.max(panelWidth, P4gTheme.dp(activity, 220)));
            float panelHeight = Math.min(canvas.getHeight() - edge * 2.0f,
                    lines.length * lineHeight + pad * 1.4f + graphHeight);
            float left = edge;
            float top = edge;
            canvas.drawRoundRect(left, top, left + panelWidth, top + panelHeight, pad, pad, background);
            float y = top + pad + paint.getTextSize();
            for (String line : lines) { canvas.drawText(line, left + pad, y, paint); y += lineHeight; }
            if (frameTime && frameTimes.length > 0) {
                float bottom = top + panelHeight - pad;
                float graphTop = top + lines.length * lineHeight + pad;
                float height = Math.max(0, bottom - graphTop);
                float graphWidth = Math.max(1, panelWidth - pad * 2);
                float maximum = 50;
                for (float ms : frameTimes) maximum = Math.max(maximum, ms);
                paint.setColor(0x668D99AD);
                canvas.drawLine(left + pad, bottom - height * 33.333f / maximum,
                        left + pad + graphWidth, bottom - height * 33.333f / maximum, paint);
                paint.setColor(P4gTheme.YELLOW);
                paint.setStrokeWidth(Math.max(1, P4gTheme.dp(activity, 1)));
                for (int i = 1; i < frameTimes.length; i++) {
                    float x0 = left + pad + graphWidth * (i - 1) / 119.0f;
                    float x1 = left + pad + graphWidth * i / 119.0f;
                    canvas.drawLine(x0, bottom - height * frameTimes[i - 1] / maximum,
                            x1, bottom - height * frameTimes[i] / maximum, paint);
                }
            }
        } catch (IllegalArgumentException ignored) {
        } finally {
            if (canvas != null) holder.unlockCanvasAndPost(canvas);
        }
    }

    private void clear() {
        if (!surfaceReady || panel == null) return;
        Canvas canvas = null;
        try {
            canvas = panel.getHolder().lockCanvas();
            if (canvas != null) canvas.drawColor(Color.TRANSPARENT, PorterDuff.Mode.CLEAR);
        } catch (IllegalArgumentException ignored) {
        } finally {
            if (canvas != null) panel.getHolder().unlockCanvasAndPost(canvas);
        }
    }

    private static void append(StringBuilder text, String line) {
        if (text.length() != 0) text.append('\n');
        text.append(line);
    }

    private static String read(String path) throws Exception {
        try (BufferedReader in = new BufferedReader(new FileReader(path))) {
            String line = in.readLine();
            if (line == null) throw new java.io.IOException("Empty counter");
            return line.trim();
        }
    }

    private String gpuUsage() {
        if (gpuUnavailable) return "N/A";
        try {
            if (gpuCounter == null) gpuCounter = findGpuCounter();
            if (gpuCounter == null) { gpuUnavailable = true; return "N/A"; }
            double value = parseGpuUsage(read(gpuCounter));
            if (value < 0.0) { gpuUnavailable = true; return "N/A"; }
            return String.format(Locale.US, "%.0f%%", value);
        } catch (Exception ignored) { gpuUnavailable = true; return "N/A"; }
    }

    /** Finds readable Adreno KGSL or Mali/devfreq utilization without assuming an SoC-specific address. */
    private static String findGpuCounter() {
        String[] direct = {
            "/sys/class/kgsl/kgsl-3d0/gpubusy",
            "/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage",
            "/sys/class/misc/mali0/device/utilization",
            "/sys/class/misc/mali0/device/utilisation",
            "/sys/class/misc/mali0/device/load",
            "/sys/class/misc/mali0/device/gpu_load",
            "/sys/kernel/gpu/gpu_busy",
            "/sys/devices/platform/mali/utilization",
            "/sys/devices/platform/gpusysfs/gpu_busy"
        };
        for (String path : direct) if (validGpuCounter(path)) return path;

        // Mali device addresses differ between Exynos/MediaTek chips. Discover the devfreq node by name.
        File root = new File("/sys/class/devfreq");
        File[] nodes = root.listFiles();
        if (nodes == null) return null;
        for (File node : nodes) {
            String identity = node.getName().toLowerCase(Locale.US);
            try { identity += " " + read(new File(node, "name").getPath()).toLowerCase(Locale.US); }
            catch (Exception ignored) { }
            if (!(identity.contains("mali") || identity.contains("gpu"))) continue;
            for (String field : new String[] {"load", "utilization", "utilisation", "gpu_load", "gpu_busy"}) {
                String path = new File(node, field).getPath();
                if (validGpuCounter(path)) return path;
            }
        }
        return null;
    }

    private static boolean validGpuCounter(String path) {
        try { return parseGpuUsage(read(path)) >= 0.0; }
        catch (Exception ignored) { return false; }
    }

    /** Accepts percentages, Mali's "load@frequency" and KGSL's "busy total" formats. */
    private static double parseGpuUsage(String raw) {
        String[] fields = raw.trim().split("[^0-9.]+");
        double first = -1.0, second = -1.0;
        for (String field : fields) {
            if (field.length() == 0) continue;
            try {
                if (first < 0.0) first = Double.parseDouble(field);
                else { second = Double.parseDouble(field); break; }
            } catch (NumberFormatException ignored) { }
        }
        if (first < 0.0) return -1.0;
        if (raw.indexOf('@') >= 0 || raw.indexOf('%') >= 0 || second < 0.0)
            return first <= 100.0 ? first : -1.0;
        if (second <= 0.0 || first > second) return -1.0;
        return Math.min(100.0, first * 100.0 / second);
    }

    private static String residentMemory() {
        try {
            String[] fields = read("/proc/self/statm").split("\\s+");
            long pages = Long.parseLong(fields[1]);
            long pageSize = Os.sysconf(OsConstants._SC_PAGESIZE);
            if (pageSize <= 0 || pages < 0) return "N/A";
            return String.format(Locale.US, "%.0f MiB", pages * (double) pageSize / 1048576.0);
        } catch (Exception ignored) { return "N/A"; }
    }

    void stop() {
        ++generation;
        if (worker != null) worker.removeCallbacksAndMessages(null);
        if (thread != null) thread.quitSafely();
        worker = null; thread = null;
        clear();
        if (panel != null) panel.setVisibility(View.GONE);
    }

    void close() { stop(); }
}
