Scriptname Helios_MCM extends SKI_ConfigBase

Int OID_MinSecondsForLoadFadeIn = -1
Int OID_FadeToBlackFadeSeconds = -1
Int OID_AutoCSTonemapping = -1
Int OID_WeatherSyncNotifications = -1
Int OID_WeatherSyncDetailedLogging = -1
Int OID_WeatherSyncSpeedLogging = -1

String LoadFadeInINI = "fMinSecondsForLoadFadeIn:Interface"
String FadeToBlackINI = "fFadeToBlackFadeSeconds:Interface"
String WeatherSyncProfile = "Helios"
Float DefaultLoadFadeInSeconds = 1.5
Float DefaultFadeToBlackFadeSeconds = 1.0
Float LoadFadeInSeconds = 1.5
Float FadeToBlackFadeSeconds = 1.0
Bool InterfaceIniSettingsInitialized = False
Bool AutoCSTonemapping = True
Bool WeatherSyncNotifications = False
Bool WeatherSyncDetailedLogging = False
Bool WeatherSyncSpeedLogging = False

Event OnConfigInit()
    Pages = new String[2]
    Pages[0] = "Settings"
    Pages[1] = "Debug"

    LoadFadeInSeconds = Utility.GetINIFloat(LoadFadeInINI)
    FadeToBlackFadeSeconds = Utility.GetINIFloat(FadeToBlackINI)
    InterfaceIniSettingsInitialized = True
    RefreshWeatherSyncSettings()
EndEvent

Event OnConfigOpen()
    If !InterfaceIniSettingsInitialized
        LoadFadeInSeconds = Utility.GetINIFloat(LoadFadeInINI)
        FadeToBlackFadeSeconds = Utility.GetINIFloat(FadeToBlackINI)
        InterfaceIniSettingsInitialized = True
    EndIf

    Utility.SetINIFloat(LoadFadeInINI, LoadFadeInSeconds)
    Utility.SetINIFloat(FadeToBlackINI, FadeToBlackFadeSeconds)
    RefreshWeatherSyncSettings()
EndEvent

Event OnPageReset(string page)
    SetCursorFillMode(TOP_TO_BOTTOM)
    If page == "" || page == "Settings"
        ClearOptionIDs()
        DrawSettingsPage()
    ElseIf page == "Debug"
        ClearOptionIDs()
        DrawDebugPage()
    EndIf
EndEvent

Function ClearOptionIDs()
    OID_MinSecondsForLoadFadeIn = -1
    OID_FadeToBlackFadeSeconds = -1
    OID_AutoCSTonemapping = -1
    OID_WeatherSyncNotifications = -1
    OID_WeatherSyncDetailedLogging = -1
    OID_WeatherSyncSpeedLogging = -1
EndFunction

Function DrawSettingsPage()
    OID_AutoCSTonemapping = AddToggleOption("Auto CS Tonemapping", AutoCSTonemapping)
    AddTextOption(SmallText("Automatically enables CS Tonemapping for Helios image spaces when another mod using it is detected. Restart required."), "", OPTION_FLAG_DISABLED)
    AddEmptyOption()
    AddTextOption(SmallText("If you are experiencing sudden lighting changes after loading an interior due to script lag,"), "", OPTION_FLAG_DISABLED)
    AddTextOption(SmallText("you will need to increase the duration of the load screen to hide it."), "", OPTION_FLAG_DISABLED)
    AddTextOption(SmallText("Values are in seconds."), "", OPTION_FLAG_DISABLED)
    OID_MinSecondsForLoadFadeIn = AddSliderOption("Load Screen Duration", LoadFadeInSeconds, "{1} s")
    AddEmptyOption()
    AddTextOption(SmallText("You can lower the duration of the fade to compensate for the longer load screen."), "", OPTION_FLAG_DISABLED)
    OID_FadeToBlackFadeSeconds = AddSliderOption("Fade from Black Duration", FadeToBlackFadeSeconds, "{1} s")
EndFunction

Function DrawDebugPage()
    AddHeaderOption("Weather Sync")
    OID_WeatherSyncNotifications = AddToggleOption("Notifications", WeatherSyncNotifications)
    OID_WeatherSyncDetailedLogging = AddToggleOption("Detailed Logging", WeatherSyncDetailedLogging)
    OID_WeatherSyncSpeedLogging = AddToggleOption("Speed Logs", WeatherSyncSpeedLogging)
EndFunction

Function RefreshWeatherSyncSettings()
    AutoCSTonemapping = Heliosphan.GetAutoCSTonemapping(WeatherSyncProfile)
    WeatherSyncNotifications = Heliosphan.GetWeatherSyncNotifications(WeatherSyncProfile)
    WeatherSyncDetailedLogging = Heliosphan.GetWeatherSyncDetailedLogging(WeatherSyncProfile)
    WeatherSyncSpeedLogging = Heliosphan.GetWeatherSyncSpeedLogging(WeatherSyncProfile)
EndFunction

Event OnOptionSelect(int Option)
    Bool Saved = False
    Bool PreviousValue = AutoCSTonemapping
    If Option == OID_AutoCSTonemapping
        AutoCSTonemapping = !AutoCSTonemapping
        Saved = Heliosphan.SetAutoCSTonemapping(WeatherSyncProfile, AutoCSTonemapping)
        If !Saved
            AutoCSTonemapping = PreviousValue
            Debug.Notification("Helios: Could not update Helios.json")
        EndIf
        SetToggleOptionValue(Option, AutoCSTonemapping)
    ElseIf Option == OID_WeatherSyncNotifications
        WeatherSyncNotifications = !WeatherSyncNotifications
        Saved = Heliosphan.SetWeatherSyncNotifications(WeatherSyncProfile, WeatherSyncNotifications)
        SetToggleOptionValue(Option, WeatherSyncNotifications)
        If !Saved
            Debug.Notification("Helios: Could not update HeliosphanSettings.json")
        EndIf
    ElseIf Option == OID_WeatherSyncDetailedLogging
        WeatherSyncDetailedLogging = !WeatherSyncDetailedLogging
        Saved = Heliosphan.SetWeatherSyncDetailedLogging(WeatherSyncProfile, WeatherSyncDetailedLogging)
        SetToggleOptionValue(Option, WeatherSyncDetailedLogging)
        If !Saved
            Debug.Notification("Helios: Could not update HeliosphanSettings.json")
        EndIf
    ElseIf Option == OID_WeatherSyncSpeedLogging
        WeatherSyncSpeedLogging = !WeatherSyncSpeedLogging
        Saved = Heliosphan.SetWeatherSyncSpeedLogging(WeatherSyncProfile, WeatherSyncSpeedLogging)
        SetToggleOptionValue(Option, WeatherSyncSpeedLogging)
        If !Saved
            Debug.Notification("Helios: Could not update HeliosphanSettings.json")
        EndIf
    EndIf
EndEvent

Event OnOptionSliderOpen(int Option)
    If Option == OID_MinSecondsForLoadFadeIn
        SetSliderDialogStartValue(LoadFadeInSeconds)
        SetSliderDialogDefaultValue(DefaultLoadFadeInSeconds)
        SetSliderDialogRange(0.0, 10.0)
        SetSliderDialogInterval(0.1)
    ElseIf Option == OID_FadeToBlackFadeSeconds
        SetSliderDialogStartValue(FadeToBlackFadeSeconds)
        SetSliderDialogDefaultValue(DefaultFadeToBlackFadeSeconds)
        SetSliderDialogRange(0.0, 10.0)
        SetSliderDialogInterval(0.1)
    EndIf
EndEvent

Event OnOptionSliderAccept(int Option, float Value)
    If Option == OID_MinSecondsForLoadFadeIn
        LoadFadeInSeconds = Value
        Utility.SetINIFloat(LoadFadeInINI, LoadFadeInSeconds)
        SetSliderOptionValue(Option, LoadFadeInSeconds, "{1} s")
    ElseIf Option == OID_FadeToBlackFadeSeconds
        FadeToBlackFadeSeconds = Value
        Utility.SetINIFloat(FadeToBlackINI, FadeToBlackFadeSeconds)
        SetSliderOptionValue(Option, FadeToBlackFadeSeconds, "{1} s")
    EndIf
EndEvent

Event OnOptionDefault(int Option)
    If Option == OID_AutoCSTonemapping
        AutoCSTonemapping = True
        Heliosphan.SetAutoCSTonemapping(WeatherSyncProfile, AutoCSTonemapping)
        SetToggleOptionValue(Option, AutoCSTonemapping)
    ElseIf Option == OID_MinSecondsForLoadFadeIn
        LoadFadeInSeconds = DefaultLoadFadeInSeconds
        Utility.SetINIFloat(LoadFadeInINI, LoadFadeInSeconds)
        SetSliderOptionValue(Option, LoadFadeInSeconds, "{1} s")
    ElseIf Option == OID_FadeToBlackFadeSeconds
        FadeToBlackFadeSeconds = DefaultFadeToBlackFadeSeconds
        Utility.SetINIFloat(FadeToBlackINI, FadeToBlackFadeSeconds)
        SetSliderOptionValue(Option, FadeToBlackFadeSeconds, "{1} s")
    ElseIf Option == OID_WeatherSyncNotifications
        WeatherSyncNotifications = False
        Heliosphan.SetWeatherSyncNotifications(WeatherSyncProfile, WeatherSyncNotifications)
        SetToggleOptionValue(Option, WeatherSyncNotifications)
    ElseIf Option == OID_WeatherSyncDetailedLogging
        WeatherSyncDetailedLogging = False
        Heliosphan.SetWeatherSyncDetailedLogging(WeatherSyncProfile, WeatherSyncDetailedLogging)
        SetToggleOptionValue(Option, WeatherSyncDetailedLogging)
    ElseIf Option == OID_WeatherSyncSpeedLogging
        WeatherSyncSpeedLogging = False
        Heliosphan.SetWeatherSyncSpeedLogging(WeatherSyncProfile, WeatherSyncSpeedLogging)
        SetToggleOptionValue(Option, WeatherSyncSpeedLogging)
    EndIf
EndEvent

Event OnOptionHighlight(int Option)
    If Option == OID_AutoCSTonemapping
        SetInfoText("Automatically enables CS Tonemapping for Helios image spaces when another mod using it is detected. Restart required.")
    ElseIf Option == OID_MinSecondsForLoadFadeIn
        SetInfoText("Default is 1.5 seconds")
    ElseIf Option == OID_FadeToBlackFadeSeconds
        SetInfoText("Default is 1.0 second")
    ElseIf Option == OID_WeatherSyncNotifications
        SetInfoText("Enables debug messages that appear when entering or leaving a Helios interior.")
    ElseIf Option == OID_WeatherSyncDetailedLogging
        SetInfoText("Saves detailed diagnostic logging to Heliosphan.log.")
    ElseIf Option == OID_WeatherSyncSpeedLogging
        SetInfoText("Saves startup, game-load, and cell timing to the Luma Suite logs. Restart to capture startup timing.")
    EndIf
EndEvent

String Function SmallText(String Text)
    Return "<font size='17'>" + Text + "</font>"
EndFunction
