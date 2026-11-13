package io.github.p4grecomp;

public final class LauncherSettingsTest {
    public static void main(String[] args) {
        Launcher.Option option = new Launcher.Option("NX_PIPE_PREWARM_LOAD", "Carga", "Load",
                new String[] {"light", "standard"}, new String[] {"Leve", "Padrão"},
                new String[] {"Light", "Standard"}, 1);
        option.select("light");
        check(option.pick == 0);
        option.select("invalid-old-profile-value");
        check(option.pick == 1);
        option.select(null);
        check(option.pick == option.defaultPick);
        check(Launcher.optionGroup("NX_OVERLAY_FRAME_TIME").equals("overlay"));
        check(Launcher.optionGroup("NX_PIPE_PREWARM_LOAD").equals("graphics"));
        check(Launcher.optionGroup("NX_TOUCH_OPACITY").equals("controls"));
        check(Launcher.optionGroup("NX_TOUCH_SHOW").equals("quick"));
        check(Launcher.optionGroup("NX_TRACE_PERF").equals("logs"));
        System.out.println("Launcher settings tests passed");
    }
    private static void check(boolean value) { if (!value) throw new AssertionError(); }
}
