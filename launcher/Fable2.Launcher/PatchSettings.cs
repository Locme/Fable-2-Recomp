namespace Fable2Launcher;

// Game patch toggles the launcher exposes. They live in the recomp's own
// config (fable2_config.toml [patches]), not in the SDK cvar file fable_2.toml.
public static class PatchSettings
{
    public const string ConfigFileName = "fable2_config.toml";
    public const string Section = "patches";

    public static readonly string[] ManagedKeys = ["disable_motion_blur", "skip_intro_videos"];

    public static Dictionary<string, string> Create(bool disableMotionBlur, bool skipIntroVideos) =>
        new(StringComparer.OrdinalIgnoreCase)
        {
            ["disable_motion_blur"] = disableMotionBlur.ToString().ToLowerInvariant(),
            ["skip_intro_videos"] = skipIntroVideos.ToString().ToLowerInvariant()
        };
}
