Scriptname Helios_MCM extends SKI_ConfigBase

Int OID_WeatherSyncNotifications = -1
Int OID_WeatherSyncDetailedLogging = -1
Int OID_WeatherSyncSpeedLogging = -1

String WeatherSyncProfile = "Helios"
Bool WeatherSyncNotifications = False
Bool WeatherSyncDetailedLogging = False
Bool WeatherSyncSpeedLogging = False

Int Function GetVersion()
    Return 4
EndFunction

Event OnVersionUpdate(Int NewVersion)
    InitializePages()
EndEvent

Function InitializePages()
    Pages = new String[1]
    Pages[0] = "Settings"
EndFunction

Event OnConfigInit()
    InitializePages()
    RefreshWeatherSyncSettings()
EndEvent

Event OnConfigOpen()
    InitializePages()
    RefreshWeatherSyncSettings()
EndEvent

Event OnPageReset(string page)
    SetCursorFillMode(TOP_TO_BOTTOM)
    If page == "" || page == "Settings"
        ClearOptionIDs()
        DrawSettingsPage()
    EndIf
EndEvent

Function ClearOptionIDs()
    OID_WeatherSyncNotifications = -1
    OID_WeatherSyncDetailedLogging = -1
    OID_WeatherSyncSpeedLogging = -1
EndFunction

Function DrawSettingsPage()
    String HeliosInteriorStatus = "No"
    If Heliosphan.IsCurrentHeliosInterior()
        HeliosInteriorStatus = "Yes"
    EndIf

    AddHeaderOption("Status")
    AddTextOption("Weather", Heliosphan.GetCurrentWeatherStatus(), OPTION_FLAG_DISABLED)
    AddTextOption("Region", Heliosphan.GetCurrentRegionStatus(), OPTION_FLAG_DISABLED)
    AddTextOption("Helios Interior", HeliosInteriorStatus, OPTION_FLAG_DISABLED)
    AddEmptyOption()
    AddHeaderOption("Debug")
    OID_WeatherSyncNotifications = AddToggleOption("Notifications", WeatherSyncNotifications)
    OID_WeatherSyncDetailedLogging = AddToggleOption("Detailed Logging", WeatherSyncDetailedLogging)
    OID_WeatherSyncSpeedLogging = AddToggleOption("Speed Logs", WeatherSyncSpeedLogging)
EndFunction

Function RefreshWeatherSyncSettings()
    WeatherSyncNotifications = Heliosphan.GetWeatherSyncNotifications(WeatherSyncProfile)
    WeatherSyncDetailedLogging = Heliosphan.GetWeatherSyncDetailedLogging(WeatherSyncProfile)
    WeatherSyncSpeedLogging = Heliosphan.GetWeatherSyncSpeedLogging(WeatherSyncProfile)
EndFunction

Event OnOptionSelect(int Option)
    Bool Saved = False
    If Option == OID_WeatherSyncNotifications
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

Event OnOptionDefault(int Option)
    If Option == OID_WeatherSyncNotifications
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
    If Option == OID_WeatherSyncNotifications
        SetInfoText("Enables debug messages that appear when entering or leaving a Helios interior.")
    ElseIf Option == OID_WeatherSyncDetailedLogging
        SetInfoText("Saves detailed diagnostic logging to Heliosphan.log.")
    ElseIf Option == OID_WeatherSyncSpeedLogging
        SetInfoText("Saves startup, game-load, and cell timing to the Luma Suite logs. Restart to capture startup timing.")
    EndIf
EndEvent
