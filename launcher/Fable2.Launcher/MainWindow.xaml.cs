using Microsoft.Win32;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Threading;

namespace Fable2Launcher;

public partial class MainWindow : Window
{
    private readonly Dictionary<string, string> _values = new(StringComparer.OrdinalIgnoreCase);
    private string? _gameDirectory;
    private string? _executableDirectory;
    private bool _loading = true;

    public MainWindow()
    {
        InitializeComponent();
        SetDefaults();
        DetectGameDirectory();
        _loading = false;
        RefreshSummary();
    }

    private void SetDefaults()
    {
        SelectByTag(ResolutionCombo, "1080p");
        SelectByTag(RenderScaleCombo, "2");
        SelectByTag(AnisotropicCombo, "5");
        SelectByTag(AntiAliasingCombo, "none");
        SelectByTag(DisplayModeCombo, "borderless");
        if (FrameLimitCombo.Items.Count == 0)
            foreach (var option in Constants.GraphicsOptions.FrameLimits)
                FrameLimitCombo.Items.Add(new ComboBoxItem { Tag = option.Value, Content = option.Label });
        SelectByTag(FrameLimitCombo, "0");
        VsyncCheck.IsChecked = true;
        DisableMotionBlurCheck.IsChecked = false;
        SkipIntroVideosCheck.IsChecked = false;
    }

    private void DetectGameDirectory()
    {
        string launcherDirectory = AppContext.BaseDirectory;
        string? configuredPath = LauncherState.LoadGameDirectory();
        if (!string.IsNullOrWhiteSpace(configuredPath) &&
            File.Exists(Path.Combine(configuredPath, "default.xex")) &&
            (File.Exists(Path.Combine(launcherDirectory, "fable_2.exe")) ||
             File.Exists(Path.Combine(configuredPath, "fable_2.exe"))))
        {
            SetGameDirectory(configuredPath);
            return;
        }
        // No saved (or no longer valid) game folder: default the selected location
        // to the folder the launcher is running from, so it is never empty on first
        // launch. The status line then explains what is still missing (e.g. that
        // default.xex has not been extracted yet).
        SetGameDirectory(launcherDirectory);
    }

    private void SetGameDirectory(string directory)
    {
        _gameDirectory = Path.GetFullPath(directory);
        _executableDirectory = File.Exists(Path.Combine(AppContext.BaseDirectory, "fable_2.exe"))
            ? AppContext.BaseDirectory : _gameDirectory;
        GamePathText.Text = _gameDirectory;
        LauncherState.SaveGameDirectory(_gameDirectory);
        try
        {
            LoadConfiguration();
            GameCompatibility compatibility = GameCompatibilityInspector.Inspect(_gameDirectory);
            GameLaunchPlanner.Create(_executableDirectory, _gameDirectory, compatibility);
            StatusText.Text = compatibility.Message;
            ConfigStateText.Text = compatibility.Label;
        }
        catch (Exception exception)
        {
            StatusText.Text = exception.Message;
            ConfigStateText.Text = "Not ready to launch";
        }
    }

    private void LoadConfiguration()
    {
        if (_gameDirectory is null) return;

        _loading = true;
        try
        {
            _values.Clear();
            SetDefaults();
            foreach ((string key, string value) in LauncherConfigFile.ReadLauncherValues(_executableDirectory!))
                _values[key] = value;

            string outputResolution = (GetValue("window_width", ""), GetValue("window_height", "")) switch
            {
                ("1280", "720") => "720p",
                ("1920", "1080") => "1080p",
                ("2560", "1440") => "1440p",
                ("3840", "2160") => "4k",
                _ => Unquote(GetValue("resolution", "1080p"))
            };
            SelectByTag(ResolutionCombo, outputResolution);
            SelectByTag(RenderScaleCombo, GetValue("resolution_scale", "2"));
            SelectByTag(AnisotropicCombo, GetValue("anisotropic_override", "5"));
            SelectByTag(AntiAliasingCombo, Unquote(GetValue("swap_post_effect", "none")));
            SelectByTag(FrameLimitCombo, GetValue("frame_limit", "0"));

            bool fullscreen = ParseBool(GetValue("fullscreen", "true"), true);
            bool exclusive = ParseBool(GetValue("fullscreen_exclusive", "false"), false);
            SelectByTag(DisplayModeCombo, !fullscreen ? "windowed" : exclusive ? "exclusive" : "borderless");
            VsyncCheck.IsChecked = ParseBool(GetValue("vsync", "true"), true);

            Dictionary<string, string> patches = LauncherConfigFile.ReadSectionValues(
                Path.Combine(_executableDirectory!, PatchSettings.ConfigFileName), PatchSettings.Section);
            DisableMotionBlurCheck.IsChecked = ParseBool(
                patches.GetValueOrDefault("disable_motion_blur", "false"), false);
            SkipIntroVideosCheck.IsChecked = ParseBool(
                patches.GetValueOrDefault("skip_intro_videos", "false"), false);
        }
        finally { _loading = false; }
        RefreshSummary();
        ConfigStateText.Text = "Settings loaded";
    }

    private string GetValue(string key, string fallback) =>
        _values.TryGetValue(key, out string? value) ? value : fallback;

    private static string Unquote(string value) => value.Trim().Trim('"', '\'');

    private static bool ParseBool(string value, bool fallback) =>
        bool.TryParse(Unquote(value), out bool parsed) ? parsed : fallback;

    private static string? SelectedTag(ComboBox combo) =>
        (combo.SelectedItem as ComboBoxItem)?.Tag?.ToString();

    private static void SelectByTag(ComboBox combo, string tag)
    {
        foreach (object item in combo.Items)
        {
            if (item is ComboBoxItem comboItem &&
                string.Equals(comboItem.Tag?.ToString(), tag, StringComparison.OrdinalIgnoreCase))
            {
                combo.SelectedItem = comboItem;
                return;
            }
        }
        // An unknown/missing value must not silently select the first (lowest)
        // resolution or render scale. Keep the explicit default/current choice.
    }

    private Dictionary<string, string> CollectSettings()
    {
        return GraphicsSettings.Create(SelectedTag(ResolutionCombo) ?? "1080p",
            SelectedTag(RenderScaleCombo) ?? "2", SelectedTag(AnisotropicCombo) ?? "5",
            SelectedTag(AntiAliasingCombo) ?? "none", SelectedTag(DisplayModeCombo) ?? "borderless",
            VsyncCheck.IsChecked == true, SelectedTag(FrameLimitCombo) ?? "0");
    }

    private bool SaveConfiguration()
    {
        if (_gameDirectory is null || _executableDirectory is null || !File.Exists(Path.Combine(_executableDirectory, "fable_2.exe")))
        {
            MessageBox.Show(this, "Choose the folder containing fable_2.exe first.",
                "Game not found", MessageBoxButton.OK, MessageBoxImage.Warning);
            return false;
        }

        string path = Path.Combine(_executableDirectory, "fable_2.toml");
        Dictionary<string, string> settings = CollectSettings();
        try
        {
            LauncherConfigFile.WriteValues(path, settings, GraphicsSettings.ManagedKeys);
            LauncherConfigFile.WriteValues(Path.Combine(_executableDirectory,
                "launcher-settings.toml"), settings, GraphicsSettings.ManagedKeys);
            SavePatchSettings();
            _values.Clear();
            foreach ((string key, string value) in settings) _values[key] = value;
        }
        catch (Exception exception)
        {
            MessageBox.Show(this, exception.Message, "Could not save settings",
                MessageBoxButton.OK, MessageBoxImage.Error);
            return false;
        }

        StatusText.Text = $"Saved {Path.GetFileName(path)}.";
        ConfigStateText.Text = "Settings saved";
        return true;
    }

    private void SavePatchSettings()
    {
        bool disableMotionBlur = DisableMotionBlurCheck.IsChecked == true;
        bool skipIntroVideos = SkipIntroVideosCheck.IsChecked == true;
        string path = Path.Combine(_executableDirectory!, PatchSettings.ConfigFileName);
        // The game writes the full, commented fable2_config.toml on its first
        // run. Before that, only create the file when a toggle is turned on;
        // the defaults already match the unchecked boxes.
        if (!File.Exists(path) && !disableMotionBlur && !skipIntroVideos) return;
        LauncherConfigFile.WriteSectionValues(path, PatchSettings.Section,
            PatchSettings.Create(disableMotionBlur, skipIntroVideos), PatchSettings.ManagedKeys);
    }

    private void LaunchGame()
    {
        if (_extracting) return;
        if (_gameDirectory is null || _executableDirectory is null) return;

        try
        {
            var plan = GameLaunchPlanner.Create(_executableDirectory, _gameDirectory,
                GameCompatibilityInspector.Inspect(_gameDirectory));
            if (!SaveConfiguration()) return;
            var startInfo = new ProcessStartInfo
            {
                FileName = plan.Executable,
                WorkingDirectory = plan.WorkingDirectory,
                UseShellExecute = false
            };
            startInfo.ArgumentList.Add("--game_data_root=" + plan.GameRoot);
            Process.Start(startInfo);
            StatusText.Text = "Fable II launched.";
            ConfigStateText.Text = "Game running";
            // The game is a separate process, so the launcher can close itself
            // now that it has started. Only close on a successful launch.
            Close();
        }
        catch (Exception exception)
        {
            MessageBox.Show(this, exception.Message, "Could not launch Fable II",
                MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    private void RefreshSummary(bool markDirty = false)
    {
        int scale = int.TryParse(SelectedTag(RenderScaleCombo), NumberStyles.Integer,
            CultureInfo.InvariantCulture, out int parsedScale) ? parsedScale : 1;
        InternalResolutionText.Text = $"Internal: {1280 * scale:N0} × {720 * scale:N0}";

        string output = SelectedTag(ResolutionCombo) switch
        {
            "720p" => "1280 × 720",
            "1080p" => "1920 × 1080",
            "1440p" => "2560 × 1440",
            "4k" => "3840 × 2160",
            _ => "Automatic"
        };
        OutputResolutionText.Text = $"Output: {output}";

        if (!_loading && markDirty)
        {
            ConfigStateText.Text = "Unsaved changes";
        }
    }

    private void SettingChanged(object sender, RoutedEventArgs e)
    {
        if (!_loading) RefreshSummary(markDirty: true);
    }

    private void SettingChanged(object sender, SelectionChangedEventArgs e)
    {
        if (!_loading) RefreshSummary(markDirty: true);
    }

    private void PresetClicked(object sender, RoutedEventArgs e)
    {
        string preset = (sender as Button)?.Tag?.ToString() ?? "balanced";
        switch (preset)
        {
            case "original":
                SelectByTag(ResolutionCombo, "720p");
                SelectByTag(RenderScaleCombo, "1");
                SelectByTag(AnisotropicCombo, "-1");
                PresetDescription.Text = "Original Xbox 360 rendering: 720p internal and game-controlled texture filtering.";
                break;
            case "balanced":
                SelectByTag(ResolutionCombo, "1080p");
                SelectByTag(RenderScaleCombo, "2");
                SelectByTag(AnisotropicCombo, "4");
                PresetDescription.Text = "1440p internal rendering with 8× anisotropic filtering, presented at 1080p.";
                break;
            case "enhanced":
                SelectByTag(ResolutionCombo, "4k");
                SelectByTag(RenderScaleCombo, "3");
                SelectByTag(AnisotropicCombo, "5");
                PresetDescription.Text = "True 4K internal rendering with 16× anisotropic filtering. Recommended for modern GPUs.";
                break;
            case "ultra":
                SelectByTag(ResolutionCombo, "4k");
                SelectByTag(RenderScaleCombo, "4");
                SelectByTag(AnisotropicCombo, "5");
                PresetDescription.Text = "5K internal supersampling down to 4K. Experimental and very GPU-memory intensive.";
                break;
        }
        SelectByTag(DisplayModeCombo, "borderless");
        SelectByTag(AntiAliasingCombo, "none");
        VsyncCheck.IsChecked = true;
        RefreshSummary(markDirty: true);
    }

    private void BrowseClicked(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFolderDialog
        {
            Title = "Choose original gamefiles (default.xex and data)",
            Multiselect = false
        };
        if (dialog.ShowDialog(this) == true)
        {
            if (!File.Exists(Path.Combine(dialog.FolderName, "default.xex")))
            {
                MessageBox.Show(this, "This folder does not contain default.xex.",
                    "Game not found", MessageBoxButton.OK, MessageBoxImage.Warning);
                return;
            }
            SetGameDirectory(dialog.FolderName);
        }
    }

    private bool _extracting;
    private volatile bool _stopRequested;
    // Non-null while the "unrecognized XEX" prompt is on screen; the worker
    // awaits its Task so the user can choose to extract anyways or stop.
    private TaskCompletionSource<bool>? _hashDecision;
    private readonly Dictionary<string, CheckBox> _rootChecks = new(StringComparer.OrdinalIgnoreCase);
    private long _lastUiPush;
    private double _lastPushedPercent = -1;
    private static readonly SolidColorBrush DoneBrush = new(Color.FromRgb(0x7F, 0xE0, 0xA6));
    private static readonly SolidColorBrush ItemBrush = new(Color.FromRgb(0xD0, 0xD8, 0xD2));

    private void ExtractClicked(object sender, RoutedEventArgs e)
    {
        if (_extracting) return;

        var isoDialog = new OpenFileDialog
        {
            Title = "Choose an ISO",
            Filter = "ISO (*.iso)|*.iso",
            CheckFileExists = true
        };
        if (isoDialog.ShowDialog(this) != true) return;
        string isoPath = isoDialog.FileName;

        // No destination prompt: extract straight into the folder the launcher is
        // running from, so the game data lands beside fable_2.exe and becomes
        // launchable in place.
        string outDir = AppContext.BaseDirectory;

        _extracting = true;
        _stopRequested = false;
        ExtractButton.IsEnabled = false;
        LaunchButton.IsEnabled = false;
        StopButton.IsEnabled = true;

        // Show the overlay immediately (indeterminate) so there is no perceptible
        // delay; the folder checklist and byte total fill in once the disc layout
        // has been read on the worker thread.
        ExtractionOverlay.Visibility = Visibility.Visible;
        ExtractionProgressBar.IsIndeterminate = true;
        ExtractionPercentText.Text = "…";
        ExtractionBytesText.Text = "Scanning…";
        ExtractionPhaseText.Text = "Scanning disc…";
        ExtractionFileText.Text = "Scanning for the GDFX header…";
        ExtractionFolderList.Children.Clear();
        _rootChecks.Clear();
        HashPrompt.Visibility = Visibility.Collapsed;

        Task.Run(async () =>
        {
            string? error = null;
            int files = 0;
            long bytes = 0;
            bool cancelled = false;
            try
            {
                using DiscExtraction disc = DiscExtraction.Open(isoPath, _ => { });
                List<RootItem> plan = disc.Plan.ToList();

                var progress = new ExtractionProgress(plan, () => _stopRequested);
                progress.Changed += p => Dispatcher.BeginInvoke(() => PushExtractionUi(p));
                progress.RootCompleted += name =>
                    Dispatcher.BeginInvoke(new Action(() => MarkRootDone(name)));

                // If Stop was pressed while the layout was being read, bail now.
                if (_stopRequested)
                    throw new ExtractionCancelledException();

                // The overlay is already on screen (indeterminate); now that the
                // layout is known, build the checklist and switch to the byte-
                // accurate determinate bar.
                Dispatcher.Invoke(() =>
                {
                    BuildFolderChecklist(plan);
                    ResetExtractionUi(progress);
                });

                // Extract the XEX under a temporary name so that a failed hash
                // check (or a user declining to extract) never clobbers an
                // existing default.xex already in the launcher folder.
                const string xexNewName = "default_new.xex";
                string xexNew = Path.Combine(outDir, xexNewName);
                string hash = disc.ExtractXex(outDir, _ => { }, progress, xexNewName);
                progress.SetPhase("Checking default.xex SHA-256…");
                (bool ok, string message) = GameCompatibilityInspector.CheckHash(hash);
                if (!ok)
                {
                    // The XEX is not a supported version. Instead of bailing out
                    // with a message box, show the reason on the overlay and let
                    // the user choose to extract the disc anyways (keeping the
                    // XEX) or stop.
                    var decision = new TaskCompletionSource<bool>(
                        TaskCreationOptions.RunContinuationsAsynchronously);
                    _hashDecision = decision;
                    Dispatcher.Invoke(() => ShowHashPrompt(message));
                    bool extractAnyways = await decision.Task;
                    if (ReferenceEquals(_hashDecision, decision))
                        _hashDecision = null;
                    if (!extractAnyways)
                    {
                        // Declined: remove the temporary XEX, leaving any existing
                        // default.xex in the launcher folder untouched.
                        try { File.Delete(xexNew); }
                        catch { /* best effort */ }
                        throw new ExtractionCancelledException();
                    }
                }

                // Promote the temporary XEX to default.xex (overwriting any
                // existing one) now that the extraction will continue.
                try
                {
                    File.Move(xexNew, Path.Combine(outDir, "default.xex"), overwrite: true);
                }
                catch (Exception moveEx)
                {
                    throw new InvalidDataException(
                        "Could not place default.xex: " + moveEx.Message);
                }

                (files, bytes) = disc.ExtractRemaining(outDir, _ => { }, progress);
                progress.SetPhase("Finalizing…");
            }
            catch (ExtractionCancelledException)
            {
                cancelled = true;
            }
            catch (Exception exception)
            {
                error = exception.Message;
            }

            Dispatcher.Invoke(() =>
            {
                _extracting = false;
                _stopRequested = false;
                ExtractButton.IsEnabled = true;
                LaunchButton.IsEnabled = true;
                if (cancelled)
                {
                    ExtractionOverlay.Visibility = Visibility.Collapsed;
                    ExtractionStatusText.Visibility = Visibility.Visible;
                    ExtractionStatusText.Text = "Extraction stopped.";
                    return;
                }
                if (error is not null)
                {
                    ExtractionOverlay.Visibility = Visibility.Collapsed;
                    ExtractionStatusText.Visibility = Visibility.Visible;
                    ExtractionStatusText.Text = "Extraction failed.";
                    MessageBox.Show(this, error, "Could not extract ISO",
                        MessageBoxButton.OK, MessageBoxImage.Error);
                    return;
                }
                ExtractionProgressBar.Value = 100;
                ExtractionPercentText.Text = "100%";
                ExtractionPhaseText.Text = "Done";
                ExtractionStatusText.Visibility = Visibility.Visible;
                ExtractionStatusText.Text =
                    $"Extracted {files:N0} files ({DiscExtraction.FormatSize(bytes)}) from {Path.GetFileName(isoPath)}.";
                // SetGameDirectory reports readiness (including whether fable_2.exe
                // is staged) in the status line and configuration badge.
                SetGameDirectory(outDir);
                HideOverlayAfter(900);
            });
        });
    }

    private void StopClicked(object sender, RoutedEventArgs e)
    {
        if (!_extracting) return;
        if (_hashDecision is not null)
        {
            DeclineHashDecision();
            return;
        }
        _stopRequested = true;
        StopButton.IsEnabled = false;
        ExtractionPhaseText.Text = "Stopping…";
    }

    // The large "Cancel" button on the "unrecognized XEX" prompt: declines the
    // extraction (same as the top Stop button while the prompt is showing).
    private void CancelClicked(object sender, RoutedEventArgs e)
    {
        DeclineHashDecision();
    }

    // Decline the "unrecognized XEX" prompt: the worker deletes the XEX and
    // reports the extraction as stopped.
    private void DeclineHashDecision()
    {
        if (_hashDecision is null) return;
        _hashDecision.TrySetResult(false);
        StopButton.IsEnabled = false;
        HashPrompt.Visibility = Visibility.Collapsed;
        ExtractionPhaseText.Text = "Stopping…";
    }

    private void ExtractAnywaysClicked(object sender, RoutedEventArgs e)
    {
        if (_hashDecision is null) return;
        _hashDecision.TrySetResult(true);
        HashPrompt.Visibility = Visibility.Collapsed;
        ExtractionPhaseText.Text = "Extracting…";
    }

    // Show the "unrecognized XEX" prompt on the overlay (called on the UI
    // thread via Dispatcher.Invoke from the worker).
    private void ShowHashPrompt(string message)
    {
        HashPromptText.Text = message;
        ExtractionPhaseText.Text = "Unrecognized default.xex";
        StopButton.IsEnabled = true;   // the user can still abort
        HashPrompt.Visibility = Visibility.Visible;
    }

    private void BuildFolderChecklist(List<RootItem> plan)
    {
        ExtractionFolderList.Children.Clear();
        _rootChecks.Clear();
        foreach (RootItem item in plan)
        {
            var box = new CheckBox
            {
                Content = item.Name,
                IsChecked = false,
                IsHitTestVisible = false,   // display-only progress checkbox
                FontSize = 14,
                FontWeight = item.IsDirectory ? FontWeights.SemiBold : FontWeights.Normal,
                Foreground = ItemBrush,
                Margin = new Thickness(0, 3, 0, 3),
                VerticalContentAlignment = VerticalAlignment.Center
            };
            ExtractionFolderList.Children.Add(box);
            _rootChecks[item.Name] = box;
        }
    }

    private void ResetExtractionUi(ExtractionProgress progress)
    {
        _lastPushedPercent = -1;
        ExtractionProgressBar.IsIndeterminate = false;
        ExtractionProgressBar.Value = 0;
        ExtractionPercentText.Text = "0%";
        ExtractionBytesText.Text =
            $"{DiscExtraction.FormatSize(0)} / {DiscExtraction.FormatSize(progress.TotalBytes)}";
        ExtractionPhaseText.Text = "Reading disc…";
        ExtractionFileText.Text = "Reading disc…";
    }

    private void PushExtractionUi(ExtractionProgress progress)
    {
        long now = Environment.TickCount64;
        double pct = progress.Percent;
        bool force = pct >= 100.0 || now - _lastUiPush >= 80 || pct - _lastPushedPercent >= 0.25;
        if (!force) return;
        _lastUiPush = now;
        _lastPushedPercent = pct;

        ExtractionProgressBar.Value = pct;
        ExtractionPercentText.Text = $"{pct:0}%";
        ExtractionBytesText.Text =
            $"{DiscExtraction.FormatSize(progress.CompletedBytes + progress.CurrentFileWritten)}"
            + $" / {DiscExtraction.FormatSize(progress.TotalBytes)}";
        ExtractionPhaseText.Text = progress.Phase;
        ExtractionFileText.Text = progress.CurrentFile ?? "…";
    }

    private void MarkRootDone(string name)
    {
        if (!_rootChecks.TryGetValue(name, out CheckBox? box)) return;
        box.IsChecked = true;
        box.Foreground = DoneBrush;
    }

    private void HideOverlayAfter(int delayMs)
    {
        var timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(delayMs) };
        timer.Tick += (_, _) =>
        {
            timer.Stop();
            if (!_extracting)   // a new extraction may have started in the meantime
                ExtractionOverlay.Visibility = Visibility.Collapsed;
        };
        timer.Start();
    }

    private void SaveClicked(object sender, RoutedEventArgs e) => SaveConfiguration();
    private void LaunchClicked(object sender, RoutedEventArgs e) => LaunchGame();
}

internal static class LauncherState
{
    private static readonly string LocalStatePath = Path.Combine(AppContext.BaseDirectory,
        "launcher-game-path.txt");

    public static string? LoadGameDirectory()
    {
        try
        {
            if (File.Exists(LocalStatePath)) return File.ReadAllText(LocalStatePath).Trim();
            return null;
        }
        catch { return null; }
    }

    public static void SaveGameDirectory(string directory)
    {
        try
        {
            File.WriteAllText(LocalStatePath, directory);
            return;
        }
        catch
        {
            // Graphics saving reports write failures separately. No global
            // fallback: different installations must not inherit other paths.
        }
    }
}
