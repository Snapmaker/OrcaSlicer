cmake_minimum_required(VERSION 3.13)
project(libslic3r_gui)

include(PrecompiledHeader)

set(SLIC3R_GUI_SOURCES
    Config/Snapshot.cpp
    Config/Snapshot.hpp
    Config/Version.cpp
    Config/Version.hpp
    GUI/2DBed.cpp
    GUI/2DBed.hpp
    GUI/3DBed.cpp
    GUI/3DBed.hpp
    GUI/3DScene.cpp
    GUI/3DScene.hpp
    GUI/AboutDialog.cpp
    GUI/AboutDialog.hpp
    GUI/AmsMappingPopup.cpp
    GUI/AmsMappingPopup.hpp
    GUI/AMSMaterialsSetting.cpp
    GUI/AMSMaterialsSetting.hpp
    GUI/AMSSetting.cpp
    GUI/AMSSetting.hpp
    GUI/AmsWidgets.cpp
    GUI/AmsWidgets.hpp
    GUI/Auxiliary.cpp
    GUI/AuxiliaryDataViewModel.cpp
    GUI/AuxiliaryDataViewModel.hpp
    GUI/AuxiliaryDialog.cpp
    GUI/AuxiliaryDialog.hpp
    GUI/Auxiliary.hpp
    GUI/BackgroundSlicingProcess.cpp
    GUI/BackgroundSlicingProcess.hpp
    GUI/BBLStatusBarBind.cpp
    GUI/BBLStatusBarBind.hpp
    GUI/BBLStatusBar.cpp
    GUI/BBLStatusBar.hpp
    GUI/BBLStatusBarSend.cpp
    GUI/BBLStatusBarSend.hpp
    GUI/BBLTopbar.cpp
    GUI/BBLTopbar.hpp
    GUI/BedShapeDialog.cpp
    GUI/BedShapeDialog.hpp
    GUI/BindDialog.cpp
    GUI/BindDialog.hpp
    GUI/BitmapCache.cpp
    GUI/BitmapCache.hpp
    GUI/BitmapComboBox.cpp
    GUI/BitmapComboBox.hpp
    GUI/BonjourDialog.cpp
    GUI/BonjourDialog.hpp
    GUI/calib_dlg.cpp
    GUI/calib_dlg.hpp
    GUI/Calibration.cpp
    GUI/Calibration.hpp
    GUI/CalibrationPanel.cpp
    GUI/CalibrationPanel.hpp
    GUI/CalibrationWizardCaliPage.cpp
    GUI/CalibrationWizardCaliPage.hpp
    GUI/CalibrationWizard.cpp
    GUI/CalibrationWizard.hpp
    GUI/CalibrationWizardPage.cpp
    GUI/CalibrationWizardPage.hpp
    GUI/CalibrationWizardPresetPage.cpp
    GUI/CalibrationWizardPresetPage.hpp
    GUI/CalibrationWizardSavePage.cpp
    GUI/CalibrationWizardSavePage.hpp
    GUI/CalibrationWizardStartPage.cpp
    GUI/CalibrationWizardStartPage.hpp
    GUI/CaliHistoryDialog.cpp
    GUI/CaliHistoryDialog.hpp
    GUI/Camera.cpp
    GUI/Camera.hpp
    GUI/CameraPopup.cpp
    GUI/CameraPopup.hpp
    GUI/CameraUtils.cpp
    GUI/CameraUtils.hpp
    GUI/CloneDialog.cpp
    GUI/CloneDialog.hpp
    GUI/FillBedDialog.cpp
    GUI/FillBedDialog.hpp
    GUI/RemeshDialog.cpp
    GUI/RemeshDialog.hpp
    GUI/RoundDialog.cpp
    GUI/RoundDialog.hpp
    GUI/QuadRemeshDialog.cpp
    GUI/QuadRemeshDialog.hpp
    GUI/SliceBakeDialog.cpp
    GUI/SliceBakeDialog.hpp
    GUI/ColorSplitDialog.cpp
    GUI/ColorSplitDialog.hpp
    GUI/ImageFillDialog.cpp
    GUI/ImageFillDialog.hpp
    GUI/ConfigManipulation.cpp
    GUI/ConfigManipulation.hpp
    GUI/ConfigWizard.cpp
    GUI/ConfigWizard.hpp
    GUI/ConfigWizard_private.hpp
    GUI/ConnectPrinter.cpp
    GUI/ConnectPrinter.hpp
    GUI/CreatePresetsDialog.cpp
    GUI/CreatePresetsDialog.hpp
    GUI/DailyTips.cpp
    GUI/DailyTips.hpp
    GUI/DesktopIntegrationDialog.cpp
    GUI/DesktopIntegrationDialog.hpp
    GUI/DeviceManager.cpp
    GUI/DeviceManager.hpp
    GUI/Downloader.cpp
    GUI/DownloaderFileGet.cpp
    GUI/DownloaderFileGet.hpp
    GUI/Downloader.hpp
    GUI/DownloadProgressDialog.cpp
    GUI/DownloadProgressDialog.hpp
    GUI/DragCanvas.cpp
    GUI/DragCanvas.hpp
    GUI/EditGCodeDialog.cpp
    GUI/EditGCodeDialog.hpp
    GUI/ExtraRenderers.cpp
    GUI/ExtraRenderers.hpp
    GUI/ExtrusionCalibration.cpp
    GUI/ExtrusionCalibration.hpp
    GUI/Field.cpp
    GUI/Field.hpp
    GUI/FilamentColorUtils.cpp
    GUI/FilamentColorUtils.hpp
    GUI/DualNozzleSliceDialog.cpp
    GUI/DualNozzleSliceDialog.hpp
    GUI/DualNozzleState.cpp
    GUI/DualNozzleState.hpp
    GUI/FilamentGroupDialog.cpp
    GUI/FilamentGroupDialog.hpp
    GUI/FlowTypeHelper.cpp
    GUI/FlowTypeHelper.hpp
    GUI/FlowVariantEdit.cpp
    GUI/FlowVariantEdit.hpp
    GUI/HighFlowCompat.cpp
    GUI/HighFlowCompat.hpp
    GUI/FileArchiveDialog.cpp
    GUI/FileArchiveDialog.hpp
    GUI/filamentsync/FilamentColorMapBox.cpp
    GUI/filamentsync/FilamentColorMapBox.hpp
    GUI/filamentsync/FilamentColorMapBoxGroup.cpp
    GUI/filamentsync/FilamentColorMapBoxGroup.hpp
    GUI/filamentsync/FilamentData.hpp
    GUI/filamentsync/FilamentScrollBar.cpp
    GUI/filamentsync/FilamentScrollBar.hpp
    GUI/filamentsync/FilamentSyncAlgorithm.cpp
    GUI/filamentsync/FilamentSyncAlgorithm.hpp
    GUI/filamentsync/MachineFilamentPicker.cpp
    GUI/filamentsync/MachineFilamentPicker.hpp
    GUI/filamentsync/PlaterPreview.cpp
    GUI/filamentsync/PlaterPreview.hpp
    GUI/filamentsync/SyncConfirmDialog.cpp
    GUI/filamentsync/SyncConfirmDialog.hpp
    GUI/filamentsync/SyncFilamentColorDialog.cpp
    GUI/filamentsync/SyncFilamentColorDialog.hpp
    GUI/format.hpp
    GUI/GCodeViewer.cpp
    GUI/GCodeViewer.hpp
    GUI/Gizmos/GizmoObjectManipulation.cpp
    GUI/Gizmos/GizmoObjectManipulation.hpp
    GUI/Gizmos/GLGizmoAssembly.cpp
    GUI/Gizmos/GLGizmoAssembly.hpp
    GUI/Gizmos/GLGizmoBase.cpp
    GUI/Gizmos/GLGizmoBase.hpp
    GUI/Gizmos/GLGizmoBrimEars.cpp
    GUI/Gizmos/GLGizmoBrimEars.hpp
    GUI/Gizmos/GLGizmoCut.cpp
    GUI/Gizmos/GLGizmoCut.hpp
    GUI/Gizmos/GLGizmoEmboss.cpp
    GUI/Gizmos/GLGizmoEmboss.hpp
    #GUI/Gizmos/GLGizmoFaceDetector.cpp
    #GUI/Gizmos/GLGizmoFaceDetector.hpp
    GUI/Gizmos/GLGizmoFdmSupports.cpp
    GUI/Gizmos/GLGizmoFdmSupports.hpp
    GUI/Gizmos/GLGizmoFlatten.cpp
    GUI/Gizmos/GLGizmoFlatten.hpp
    GUI/Gizmos/GLGizmoFuzzySkin.cpp
    GUI/Gizmos/GLGizmoFuzzySkin.hpp
    #GUI/Gizmos/GLGizmoHollow.cpp
    #GUI/Gizmos/GLGizmoHollow.hpp
    GUI/Gizmos/GLGizmoMeasure.cpp
    GUI/Gizmos/GLGizmoMeasure.hpp
    GUI/Gizmos/GLGizmoMeshBoolean.cpp
    GUI/Gizmos/GLGizmoMeshBoolean.hpp
    GUI/Gizmos/GLGizmoMmuSegmentation.cpp
    GUI/Gizmos/GLGizmoMmuSegmentation.hpp
    GUI/Gizmos/GLGizmoAlignment.cpp
    GUI/Gizmos/GLGizmoAlignment.hpp
    GUI/Gizmos/GLGizmoMove.cpp
    GUI/Gizmos/GLGizmoMove.hpp
    GUI/Gizmos/GLGizmoPainterBase.cpp
    GUI/Gizmos/GLGizmoPainterBase.hpp
    GUI/Gizmos/GLGizmoRotate.cpp
    GUI/Gizmos/GLGizmoRotate.hpp
    GUI/Gizmos/GLGizmoScale.cpp
    GUI/Gizmos/GLGizmoScale.hpp
    GUI/Gizmos/GLGizmosCommon.cpp
    GUI/Gizmos/GLGizmosCommon.hpp
    GUI/Gizmos/FacetPicker.cpp
    GUI/Gizmos/FacetPicker.hpp
    GUI/Gizmos/GLGizmoSeam.cpp
    GUI/Gizmos/GLGizmoSeam.hpp
    GUI/Gizmos/GLGizmoEdit.cpp
    GUI/Gizmos/GLGizmoEdit.hpp
    GUI/Gizmos/GLGizmoSculpt.cpp
    GUI/Gizmos/GLGizmoSculpt.hpp
    GUI/Gizmos/GLGizmoSimplify.cpp
    GUI/Gizmos/GLGizmoSimplify.hpp
    #GUI/Gizmos/GLGizmoSlaSupports.cpp
    #GUI/Gizmos/GLGizmoSlaSupports.hpp
    GUI/Gizmos/GLGizmosManager.cpp
    GUI/Gizmos/GLGizmosManager.hpp
    GUI/Gizmos/GLGizmoSVG.cpp
    GUI/Gizmos/GLGizmoSVG.hpp
    #GUI/Gizmos/GLGizmoText.cpp
    #GUI/Gizmos/GLGizmoText.hpp
    GUI/GLCanvas3D.cpp
    GUI/GLCanvas3D.hpp
    GUI/GLModel.cpp
    GUI/GLModel.hpp
    GUI/GLSelectionRectangle.cpp
    GUI/GLSelectionRectangle.hpp
    GUI/GLShader.cpp
    GUI/GLShader.hpp
    GUI/GLShadersManager.cpp
    GUI/GLShadersManager.hpp
    GUI/GLTexture.cpp
    GUI/GLTexture.hpp
    GUI/GLToolbar.cpp
    GUI/GLToolbar.hpp
    GUI/DarkModeBackground.cpp
    GUI/DarkModeBackground.hpp
    GUI/GUI_App.cpp
    GUI/GUI_App.hpp
    GUI/SplashAnimation.hpp
    GUI/GUI_AuxiliaryList.cpp
    GUI/GUI_AuxiliaryList.hpp
    GUI/GUI_Colors.cpp
    GUI/GUI_Colors.hpp
    GUI/GUI.cpp
    GUI/GUI_Factories.cpp
    GUI/GUI_Factories.hpp
    GUI/GUI_Geometry.cpp
    GUI/GUI_Geometry.hpp
    GUI/GUI.hpp
    GUI/GUI_Init.cpp
    GUI/GUI_Init.hpp
    GUI/GUI_ObjectLayers.cpp
    GUI/GUI_ObjectLayers.hpp
    GUI/GUI_ObjectList.cpp
    GUI/GUI_ObjectList.hpp
    GUI/GUI_ObjectSettings.cpp
    GUI/GUI_ObjectSettings.hpp
    GUI/GUI_ObjectTable.cpp
    GUI/GUI_ObjectTable.hpp
    GUI/GUI_ObjectTableSettings.cpp
    GUI/GUI_ObjectTableSettings.hpp
    GUI/GUI_Preview.cpp
    GUI/GUI_Preview.hpp
    GUI/GUI_Utils.cpp
    GUI/GUI_Utils.hpp
    GUI/HintNotification.cpp
    GUI/HintNotification.hpp
    GUI/HMS.cpp
    GUI/HMS.hpp
    GUI/PrintErrorCommands.cpp
    GUI/PrintErrorCommands.hpp
    GUI/AmsDualLayout.cpp
    GUI/AmsDualLayout.hpp
    GUI/AmsDrying.cpp
    GUI/AmsDrying.hpp
    GUI/AMSDryCtrlDialog.cpp
    GUI/AMSDryCtrlDialog.hpp
    GUI/AmsUiPreview.cpp
    GUI/AmsUiPreview.hpp
    GUI/HMSPanel.cpp
    GUI/HMSPanel.hpp
    GUI/HttpServer.cpp
    GUI/HttpServer.hpp
    GUI/PageServerSecurity.cpp
    GUI/PageServerSecurity.hpp
    GUI/UntrustedSettingsGuard.cpp
    GUI/UntrustedSettingsGuard.hpp
    GUI/I18N.cpp
    GUI/I18N.hpp
    GUI/IconManager.cpp
    GUI/IconManager.hpp
    GUI/ImageGrid.cpp
    GUI/ImageGrid.h
    GUI/ImGuiWrapper.cpp
    GUI/ImGuiWrapper.hpp
    GUI/IMSlider.cpp
    GUI/IMSlider.hpp
    GUI/IMToolbar.cpp
    GUI/IMToolbar.hpp
    GUI/InstanceCheck.cpp
    GUI/InstanceCheck.hpp
    GUI/Jobs/ArrangeJob.cpp
    GUI/Jobs/ArrangeJob.hpp
    GUI/Jobs/BevelJob.cpp
    GUI/Jobs/BevelJob.hpp
    GUI/Jobs/BindJob.cpp
    GUI/Jobs/BindJob.hpp
    GUI/Jobs/BoostThreadWorker.cpp
    GUI/Jobs/BoostThreadWorker.hpp
    GUI/Jobs/BusyCursorJob.hpp
    GUI/Jobs/ColorSplitJob.cpp
    GUI/Jobs/ColorSplitJob.hpp
    GUI/Jobs/SliceBakeJob.cpp
    GUI/Jobs/SliceBakeJob.hpp
    GUI/Jobs/SeamAutoPaintJob.cpp
    GUI/Jobs/SeamAutoPaintJob.hpp
    GUI/Jobs/QuadRemeshJob.cpp
    GUI/Jobs/QuadRemeshJob.hpp
    GUI/Jobs/CreateFontNameImageJob.cpp
    GUI/Jobs/CreateFontNameImageJob.hpp
    GUI/Jobs/CreateFontStyleImagesJob.cpp
    GUI/Jobs/CreateFontStyleImagesJob.hpp
    GUI/Jobs/EmbossJob.cpp
    GUI/Jobs/EmbossJob.hpp
    GUI/Jobs/FillBedJob.cpp
    GUI/Jobs/FillBedJob.hpp
    GUI/Jobs/Job.hpp
    GUI/Jobs/NotificationProgressIndicator.cpp
    GUI/Jobs/NotificationProgressIndicator.hpp
    GUI/Jobs/OAuthJob.cpp
    GUI/Jobs/OAuthJob.hpp
    GUI/Jobs/OrientJob.cpp
    GUI/Jobs/OrientJob.hpp
    GUI/Jobs/PlaterWorker.hpp
    GUI/Jobs/PrintJob.cpp
    GUI/Jobs/PrintJob.hpp
    GUI/Jobs/ProgressIndicator.hpp
    GUI/Jobs/RotoptimizeJob.cpp
    GUI/Jobs/RotoptimizeJob.hpp
    GUI/Jobs/SendJob.cpp
    GUI/Jobs/SendJob.hpp
    GUI/Jobs/SLAImportDialog.hpp
    GUI/Jobs/SLAImportJob.cpp
    GUI/Jobs/SLAImportJob.hpp
    GUI/Jobs/ThreadSafeQueue.hpp
    GUI/Jobs/UpgradeNetworkJob.cpp
    GUI/Jobs/UpgradeNetworkJob.hpp
    GUI/Jobs/Worker.hpp
    GUI/KBShortcutsDialog.cpp
    GUI/KBShortcutsDialog.hpp
    GUI/MainFrame.cpp
    GUI/MainFrame.hpp
    GUI/LazyPanelHolder.hpp
    GUI/MarkdownTip.cpp
    GUI/MarkdownTip.hpp
    GUI/MediaFilePanel.cpp
    GUI/MediaFilePanel.h
    GUI/MediaPlayCtrl.cpp
    GUI/MediaPlayCtrl.h
    GUI/MeshUtils.cpp
    GUI/MeshUtils.hpp
    GUI/ModelMall.cpp
    GUI/ModelMall.hpp
    GUI/MonitorBasePanel.cpp
    GUI/MonitorBasePanel.h
    GUI/Monitor.cpp
    GUI/Monitor.hpp
    GUI/SliceCompare/CompareCanvas.cpp
    GUI/SliceCompare/CompareCanvas.hpp
    GUI/SliceCompare/SliceCompareFrame.cpp
    GUI/SliceCompare/SliceCompareFrame.hpp
    GUI/MonitorPage.cpp
    GUI/MonitorPage.hpp
    GUI/Mouse3DController.cpp
    GUI/Mouse3DController.hpp
    GUI/MsgDialog.cpp
    GUI/MsgDialog.hpp
    GUI/SupportGroupsDialog.cpp
    GUI/SupportGroupsDialog.hpp
    GUI/SupportSetEditDialog.cpp
    GUI/SupportSetEditDialog.hpp
    GUI/MultiMachine.cpp
    GUI/MultiMachine.hpp
    GUI/MultiMachineManagerPage.cpp
    GUI/MultiMachineManagerPage.hpp
    GUI/MultiMachinePage.cpp
    GUI/MultiMachinePage.hpp
    GUI/MultiPrintJob.cpp
    GUI/MultiPrintJob.hpp
    GUI/MultiSendMachineModel.cpp
    GUI/MultiSendMachineModel.hpp
    GUI/MultiTaskManagerPage.cpp
    GUI/MultiTaskManagerPage.hpp
    GUI/MultiTaskModel.cpp
    GUI/MultiTaskModel.hpp
    GUI/NetworkTestDialog.cpp
    GUI/NetworkTestDialog.hpp
    GUI/Notebook.cpp
    GUI/Notebook.hpp
    GUI/NotificationManager.cpp
    GUI/NotificationManager.hpp
    GUI/OAuthDialog.cpp
    GUI/OAuthDialog.hpp
    GUI/MixedFilamentDialog.cpp
    GUI/MixedFilamentDialog.hpp
    GUI/MixedFilamentBatchDialog.cpp
    GUI/MixedFilamentBatchDialog.hpp
    GUI/MixedGradientSelector.hpp
    GUI/MixedGradientSelector.cpp
    GUI/MixedColorMatchPanel.cpp
    GUI/MixedColorMatchPanel.hpp
    GUI/MixedColorMatchHelpers.hpp
    GUI/MixedColorMatchHelpers.cpp
    GUI/MixedFilamentColorMapPanel.hpp
    GUI/MixedFilamentColorMapPanel.cpp
    GUI/MixedFilamentBadge.cpp
    GUI/MixedFilamentBadge.hpp
    GUI/ObjColorDialog.cpp
    GUI/ObjColorDialog.hpp
    GUI/FilamentColorDialog.cpp
    GUI/FilamentColorDialog.hpp
    GUI/ObjectDataViewModel.cpp
    GUI/ObjectDataViewModel.hpp
    GUI/OG_CustomCtrl.cpp
    GUI/OG_CustomCtrl.hpp
    GUI/OpenGLManager.cpp
    GUI/OpenGLManager.hpp
    GUI/OptionsGroup.cpp
    GUI/OptionsGroup.hpp
    GUI/ParamsDialog.cpp
    GUI/ParamsDialog.hpp
    GUI/ParamsPanel.cpp
    GUI/ParamsPanel.hpp
    GUI/PartPlate.cpp
    GUI/PartPlate.hpp
    GUI/PhysicalPrinterDialog.cpp
    GUI/PhysicalPrinterDialog.hpp
    GUI/Plater.cpp
    GUI/Plater.hpp
    GUI/PlateSettingsDialog.cpp
    GUI/PlateSettingsDialog.hpp
    GUI/Preferences.cpp
    GUI/Preferences.hpp
    GUI/PresetMirror.cpp
    GUI/PresetMirror.hpp
    GUI/PresetMirrorCore.cpp
    GUI/PresetMirrorCore.hpp
    GUI/PresetComboBoxes.cpp
    GUI/PresetComboBoxes.hpp
    GUI/PresetHints.cpp
    GUI/PresetHints.hpp
    GUI/PrinterCloudAuthDialog.cpp
    GUI/PrinterCloudAuthDialog.hpp
    GUI/Printer/PrinterFileSystem.cpp
    GUI/Printer/PrinterFileSystem.h
    GUI/PrinterWebView.cpp
    GUI/PrinterWebView.hpp
    GUI/StreamPanel.cpp
    GUI/StreamPanel.hpp
    GUI/HomePanel.cpp
    GUI/HomePanel.hpp
    GUI/HubHomeView.cpp
    GUI/HubHomeView.hpp
    GUI/BambuCamRelay.cpp
    GUI/BambuCamRelay.hpp
    GUI/RemoteAccess.cpp
    GUI/RemoteAccess.hpp
    GUI/RemoteControl.cpp
    GUI/RemoteControl.hpp
    GUI/LanReconnectLadder.hpp
    GUI/RemoteEvents.cpp
    GUI/RemoteEvents.hpp
    GUI/RemoteSend.cpp
    GUI/RemoteSend.hpp
    GUI/GcodeArchive.cpp
    GUI/GcodeArchive.hpp
    GUI/RemoteSnapmaker.cpp
    GUI/RemoteSnapmaker.hpp
    GUI/SnapmakerLan.cpp
    GUI/SnapmakerTaskConfig.hpp
    GUI/SnapmakerTaskConfig.cpp
    GUI/WizardNetworking.hpp
    GUI/WizardNetworking.cpp
    GUI/PluginGuard.hpp
    GUI/PluginGuard.cpp
    GUI/DeviceModelCode.hpp
    GUI/DeviceModelCode.cpp
    GUI/NozzleTempRows.hpp
    GUI/DeviceControls.hpp
    GUI/FilamentCommands.hpp
    GUI/SnapmakerLan.hpp
    GUI/RemoteHub.cpp
    GUI/RemoteHub.hpp
    GUI/HubEventDedupe.hpp
    GUI/HubPushOwner.hpp
    GUI/RemoteNotify.cpp
    GUI/RemoteNotify.hpp
    GUI/WebPush.cpp
    GUI/WebPush.hpp
    GUI/AppPush.cpp
    GUI/AppPush.hpp
    GUI/AppPushProvider.hpp
    GUI/ApnsProvider.cpp
    GUI/FcmProvider.cpp
    GUI/HostedPush.hpp
    GUI/HostedProvider.cpp
    GUI/PrintHostDialogs.cpp
    GUI/PrintHostDialogs.hpp
    GUI/PrintHostDevicesDialog.cpp
    GUI/PrintHostDevicesDialog.hpp
    GUI/PrintOptionsDialog.cpp
    GUI/PrintOptionsDialog.hpp
    GUI/PrivacyUpdateDialog.cpp
    GUI/PrivacyUpdateDialog.hpp
    GUI/ProgressStatusBar.cpp
    GUI/ProgressStatusBar.hpp
    GUI/Project.cpp
    GUI/ProjectDirtyStateManager.cpp
    GUI/ProjectDirtyStateManager.hpp
    GUI/Project.hpp
    GUI/PublishDialog.cpp
    GUI/PublishDialog.hpp
    GUI/SpoolmanDialog.cpp
    GUI/SpoolmanDialog.hpp
    # Ultra: Flashforge device stack (port of Orca-Flashforge beta)
    GUI/FlashForge/FFDeviceTab.cpp
    GUI/FlashForge/FFDeviceTab.hpp
    GUI/FlashForge/FFConnectPrinter.cpp
    GUI/FlashForge/FFConnectPrinter.hpp
    GUI/FFUtils.cpp
    GUI/FFUtils.hpp
    GUI/TitleDialog.cpp
    GUI/TitleDialog.hpp
    GUI/Widgets/FFButton.cpp
    GUI/Widgets/FFButton.hpp
    GUI/Widgets/FFCheckBox.cpp
    GUI/Widgets/FFCheckBox.hpp
    GUI/Widgets/FFPopupWindow.cpp
    GUI/Widgets/FFPopupWindow.hpp
    GUI/Widgets/FFScrollButton.cpp
    GUI/Widgets/FFScrollButton.hpp
    GUI/Widgets/FFToggleButton.cpp
    GUI/Widgets/FFToggleButton.hpp
    GUI/Widgets/NewTempInput.cpp
    GUI/Widgets/NewTempInput.hpp
    GUI/FlashForge/AmsMappingWidgets.cpp
    GUI/FlashForge/AmsMappingWidgets.hpp
    GUI/FlashForge/ComCommand.cpp
    GUI/FlashForge/ComCommand.hpp
    GUI/FlashForge/ComCommandQue.cpp
    GUI/FlashForge/ComCommandQue.hpp
    GUI/FlashForge/ComConnection.cpp
    GUI/FlashForge/ComConnection.hpp
    GUI/FlashForge/ComThreadPool.cpp
    GUI/FlashForge/ComThreadPool.hpp
    GUI/FlashForge/ComWanConn.cpp
    GUI/FlashForge/ComWanConn.hpp
    GUI/FlashForge/DeviceData.cpp
    GUI/FlashForge/DeviceData.hpp
    GUI/FlashForge/DeviceFilterPopup.cpp
    GUI/FlashForge/DeviceFilterPopup.hpp
    GUI/FlashForge/DeviceListPanel.cpp
    GUI/FlashForge/DeviceListPanel.hpp
    GUI/FlashForge/ExportLogs.cpp
    GUI/FlashForge/ExportLogs.hpp
    GUI/FlashForge/FFDiagnostics.cpp
    GUI/FlashForge/FFDiagnostics.hpp
    GUI/FlashForge/FFDiagnosticsDialog.cpp
    GUI/FlashForge/FFDiagnosticsDialog.hpp
    GUI/FlashForge/FFDownloadTool.cpp
    GUI/FlashForge/FFDownloadTool.hpp
    GUI/FlashForge/FFTextCtrl.cpp
    GUI/FlashForge/FFTextCtrl.hpp
    GUI/FlashForge/FFTitleLessDialog.cpp
    GUI/FlashForge/FFTitleLessDialog.hpp
    GUI/FlashForge/FFTransientWindow.cpp
    GUI/FlashForge/FFTransientWindow.hpp
    GUI/FlashForge/FlashNetwork.h
    GUI/FlashForge/FlashNetworkIntfc.cpp
    GUI/FlashForge/FlashNetworkIntfc.h
    GUI/FlashForge/FreeInDestructor.h
    GUI/FlashForge/GuideWebPanel.cpp
    GUI/FlashForge/GuideWebPanel.h
    GUI/FlashForge/MaterialStation.cpp
    GUI/FlashForge/MaterialStation.hpp
    GUI/FlashForge/MultiComDef.hpp
    GUI/FlashForge/MultiComEvent.cpp
    GUI/FlashForge/MultiComEvent.hpp
    GUI/FlashForge/MultiComHelper.cpp
    GUI/FlashForge/MultiComHelper.hpp
    GUI/FlashForge/MultiComMgr.cpp
    GUI/FlashForge/MultiComMgr.hpp
    GUI/FlashForge/MultiComUtils.cpp
    GUI/FlashForge/MultiComUtils.hpp
    GUI/FlashForge/MultiSendMgr.cpp
    GUI/FlashForge/MultiSendMgr.hpp
    GUI/FlashForge/PasswordCtrl.cpp
    GUI/FlashForge/PasswordCtrl.hpp
    GUI/FlashForge/PrintDevLocalFileDlg.cpp
    GUI/FlashForge/PrintDevLocalFileDlg.hpp
    GUI/FlashForge/PrinterCameraPanel.cpp
    GUI/FlashForge/PrinterCameraPanel.h
    GUI/FlashForge/PrinterErrorMsgDlg.cpp
    GUI/FlashForge/PrinterErrorMsgDlg.hpp
    GUI/FlashForge/PrinterModelPanel.cpp
    GUI/FlashForge/PrinterModelPanel.hpp
    GUI/FlashForge/SingleDeviceState.cpp
    GUI/FlashForge/SingleDeviceState.hpp
    GUI/FlashForge/Singleton.hpp
    GUI/FlashForge/TimeLapseVideoPanel.cpp
    GUI/FlashForge/TimeLapseVideoPanel.hpp
    GUI/FlashForge/TimeLapseVideoPlayDlg.cpp
    GUI/FlashForge/TimeLapseVideoPlayDlg.hpp
    GUI/FlashForge/UserNameCtrl.cpp
    GUI/FlashForge/UserNameCtrl.hpp
    GUI/FlashForge/VerifyCodeCtrl.cpp
    GUI/FlashForge/VerifyCodeCtrl.hpp
    GUI/FlashForge/VideoDownloadErrorDlg.cpp
    GUI/FlashForge/VideoDownloadErrorDlg.hpp
    GUI/FlashForge/VideoFileOperatorMsgDlg.cpp
    GUI/FlashForge/VideoFileOperatorMsgDlg.hpp
    GUI/FlashForge/WaitEvent.cpp
    GUI/FlashForge/WaitEvent.hpp
    GUI/FlashForge/WanDevMaintainThd.cpp
    GUI/FlashForge/WanDevMaintainThd.hpp
    GUI/FlashForge/WanDevSendGcodeThd.cpp
    GUI/FlashForge/WanDevSendGcodeThd.hpp
    GUI/FlashForge/WanDevTokenMgr.cpp
    GUI/FlashForge/WanDevTokenMgr.hpp
    GUI/RammingChart.cpp
    GUI/RammingChart.hpp
    GUI/RecenterDialog.cpp
    GUI/RecenterDialog.hpp
    GUI/ReleaseNote.cpp
    GUI/ReleaseNote.hpp
    GUI/RemovableDriveManager.cpp
    GUI/RemovableDriveManager.hpp
    GUI/SavePresetDialog.cpp
    GUI/SavePresetDialog.hpp
    GUI/ScaleToVolumeDialog.cpp
    GUI/ScaleToVolumeDialog.hpp
    GUI/SceneRaycaster.cpp
    GUI/SceneRaycaster.hpp
    GUI/Search.cpp
    GUI/Search.hpp
    GUI/Selection.cpp
    GUI/Selection.hpp
    GUI/SelectMachine.cpp
    GUI/SelectMachine.hpp
    GUI/SelectMachinePop.cpp
    GUI/SelectMachinePop.hpp
    GUI/SendMultiMachinePage.cpp
    GUI/SendMultiMachinePage.hpp
    GUI/SendSystemInfoDialog.cpp
    GUI/SendSystemInfoDialog.hpp
    GUI/SendToPrinter.cpp
    GUI/SendToPrinter.hpp
    GUI/SingleChoiceDialog.cpp
    GUI/SingleChoiceDialog.hpp
    GUI/SliceInfoPanel.cpp
    GUI/SliceInfoPanel.hpp
    GUI/SliceModePopup.cpp
    GUI/SliceModePopup.hpp
    GUI/SlicingProgressNotification.cpp
    GUI/SlicingProgressNotification.hpp
    GUI/StatusPanel.cpp
    GUI/StatusPanel.hpp
    GUI/StepMeshDialog.cpp
    GUI/StepMeshDialog.hpp
    GUI/SurfaceDrag.cpp
    GUI/SurfaceDrag.hpp
    GUI/Tabbook.cpp
    GUI/Tabbook.hpp
    GUI/TabButton.cpp
    GUI/TabButton.hpp
    GUI/Tab.cpp
    GUI/Tab.hpp
    GUI/TaskManager.cpp
    GUI/TaskManager.hpp
    GUI/TextLines.cpp
    GUI/TextLines.hpp
    GUI/ThumbnailView.hpp
    GUI/TickCode.cpp
    GUI/TickCode.hpp
    GUI/UnsavedChangesDialog.cpp
    GUI/UnsavedChangesDialog.hpp
    GUI/UpdateDialogs.cpp
    GUI/UpdateDialogs.hpp
    GUI/UpgradePanel.cpp
    GUI/UpgradePanel.hpp
    GUI/UserManager.cpp
    GUI/UserManager.hpp
    GUI/UserNotification.cpp
    GUI/UserNotification.hpp
    GUI/WebDownPluginDlg.cpp
    GUI/WebDownPluginDlg.hpp
    GUI/WebGuideDialog.cpp
    GUI/WebGuideDialog.hpp
    GUI/WebUserLoginDialog.cpp
    GUI/WebUserLoginDialog.hpp
    GUI/WebViewDialog.cpp
    GUI/WebViewDialog.hpp
    GUI/Widgets/AMSControl.cpp
    GUI/Widgets/AMSControl.hpp
    GUI/Widgets/AMSDualView.cpp
    GUI/Widgets/AMSDualView.hpp
    GUI/Widgets/AMSItem.cpp
    GUI/Widgets/AMSItem.hpp
    GUI/Widgets/AxisCtrlButton.cpp
    GUI/Widgets/AxisCtrlButton.hpp
    GUI/Widgets/Button.cpp
    GUI/Widgets/Button.hpp
    GUI/Widgets/CheckBox.cpp
    GUI/Widgets/CheckBox.hpp
    GUI/Widgets/ComboBox.cpp
    GUI/Widgets/ComboBox.hpp
    GUI/Widgets/DialogButtons.cpp
    GUI/Widgets/DialogButtons.hpp
    GUI/Widgets/DropDown.cpp
    GUI/Widgets/DropDown.hpp
    GUI/Widgets/ErrorMsgStaticText.cpp
    GUI/Widgets/ErrorMsgStaticText.hpp
    GUI/Widgets/FanControl.cpp
    GUI/Widgets/FanControl.hpp
    GUI/Widgets/ImageSwitchButton.cpp
    GUI/Widgets/ImageSwitchButton.hpp
    GUI/Widgets/Label.cpp
    GUI/Widgets/LabeledStaticBox.cpp
    GUI/Widgets/LabeledStaticBox.hpp
    GUI/Widgets/Label.hpp
    GUI/Widgets/PopupWindow.cpp
    GUI/Widgets/PopupWindow.hpp
    GUI/Widgets/ProgressBar.cpp
    GUI/Widgets/ProgressBar.hpp
    GUI/Widgets/ProgressDialog.cpp
    GUI/Widgets/ProgressDialog.hpp
    GUI/Widgets/RadioBox.cpp
    GUI/Widgets/RadioBox.hpp
    GUI/Widgets/RadioGroup.cpp
    GUI/Widgets/RadioGroup.hpp
    GUI/Widgets/RoundedRectangle.cpp
    GUI/Widgets/RoundedRectangle.hpp
    GUI/Widgets/Scrollbar.cpp
    GUI/Widgets/Scrollbar.hpp
    GUI/Widgets/ScrolledWindow.cpp
    GUI/Widgets/ScrolledWindow.hpp
    GUI/Widgets/SegmentedToggle.cpp
    GUI/Widgets/SegmentedToggle.hpp
    GUI/Widgets/SideButton.cpp
    GUI/Widgets/SideButton.hpp
    GUI/Widgets/SideMenuPopup.cpp
    GUI/Widgets/SideMenuPopup.hpp
    GUI/Widgets/SideTools.cpp
    GUI/Widgets/SideTools.hpp
    GUI/Widgets/SpinInput.cpp
    GUI/Widgets/SpinInput.hpp
    GUI/Widgets/StateColor.cpp
    GUI/Widgets/StateColor.hpp
    GUI/Widgets/StateHandler.cpp
    GUI/Widgets/StateHandler.hpp
    GUI/Widgets/StaticBox.cpp
    GUI/Widgets/StaticBox.hpp
    GUI/Widgets/StaticLine.cpp
    GUI/Widgets/StaticLine.hpp
    GUI/Widgets/StepCtrl.cpp
    GUI/Widgets/StepCtrl.hpp
    GUI/Widgets/SwitchButton.cpp
    GUI/Widgets/SwitchButton.hpp
    GUI/Widgets/TabCtrl.cpp
    GUI/Widgets/TabCtrl.hpp
    GUI/Widgets/TempInput.cpp
    GUI/Widgets/TempInput.hpp
    GUI/Widgets/TextInput.cpp
    GUI/Widgets/TextInput.hpp
    GUI/Widgets/WebView.cpp
    GUI/Widgets/WebView.hpp
    GUI/WipeTowerDialog.cpp
    GUI/wxExtensions.cpp
    GUI/wxExtensions.hpp
    GUI/SMPhysicalPrinterDialog.hpp
    GUI/SMPhysicalPrinterDialog.cpp
    GUI/SSWCP.cpp
    GUI/SSWCP.hpp
    GUI/WebSocketDebugServer.cpp
    GUI/WebSocketDebugServer.hpp
    GUI/DownloadManager.cpp
    GUI/DownloadManager.hpp
    GUI/GenericDownloadDialog.cpp
    GUI/GenericDownloadDialog.hpp
    GUI/Timelapse/TimelapseDownloadPopup.cpp
    GUI/Timelapse/TimelapseDownloadPopup.hpp
    GUI/WebPresetDialog.hpp
    GUI/WebPresetDialog.cpp
    GUI/WebSMUserLoginDialog.cpp
    GUI/WebSMUserLoginDialog.hpp
    GUI/WebDeviceDialog.cpp
    GUI/WebDeviceDialog.hpp
    GUI/WebPreprintDialog.hpp
    GUI/WebPreprintDialog.cpp
    GUI/WebUrlDialog.cpp
    GUI/WebUrlDialog.hpp
    pchheader.cpp
    pchheader.hpp
    Utils/ASCIIFolding.cpp
    Utils/ASCIIFolding.hpp
    Utils/AppUpdateCheck.cpp
    Utils/AppUpdateCheck.hpp
    Utils/AstroBox.cpp
    Utils/AstroBox.hpp
    Utils/bambu_networking.hpp
    Utils/BambuStudioLauncher.cpp
    Utils/BambuStudioLauncher.hpp
    Utils/Bonjour.cpp
    Utils/Bonjour.hpp
    Utils/MeshInspect.cpp
    Utils/MeshInspect.hpp
    Utils/PaintCLI.cpp
    Utils/PaintCLI.hpp
    Utils/CalibUtils.cpp
    Utils/CalibUtils.hpp
    Utils/ColorSpaceConvert.cpp
    Utils/ColorSpaceConvert.hpp
    Utils/CrealityPrint.cpp
    Utils/CrealityPrint.hpp
    Utils/Duet.cpp
    Utils/Duet.hpp
    Utils/ElegooCanvas.cpp
    Utils/ElegooCanvas.hpp
    Utils/ElegooCanvasQuery.cpp
    Utils/ElegooLink.cpp
    Utils/ElegooLink.hpp
    Utils/EmbossStyleManager.cpp
    Utils/EmbossStyleManager.hpp
    Utils/ESP3D.cpp
    Utils/ESP3D.hpp
    Utils/FileHelp.cpp
    Utils/FileHelp.hpp
    Utils/FileDecrypt.cpp
    Utils/FileDecrypt.hpp
    Utils/FixModelByWin10.cpp
    Utils/FixModelByWin10.hpp
    Utils/FlashAir.cpp
    Utils/FlashAir.hpp
    Utils/Flashforge.cpp
    Utils/Flashforge.hpp
    Utils/FlashforgeLocalApi.hpp
    Utils/FontConfigHelp.cpp
    Utils/FontConfigHelp.hpp
    Utils/Frustum.cpp
    Utils/Frustum.hpp
    Utils/HexFile.cpp
    Utils/HexFile.hpp
    Utils/Http.cpp
    Utils/Spoolman.cpp
    Utils/Spoolman.hpp
    Utils/Http.hpp
    Utils/InstanceID.cpp
    Utils/InstanceID.hpp
    Utils/LoginUserAgent.cpp
    Utils/LoginUserAgent.hpp
    Utils/HubHomeLogic.cpp
    Utils/HubHomeLogic.hpp
    Utils/MainThreadGate.cpp
    Utils/MainThreadGate.hpp
    Utils/ServerLifetime.cpp
    Utils/ServerLifetime.hpp
    Utils/SnapmakerSilentLogin.cpp
    Utils/SnapmakerSilentLogin.hpp
    Utils/json_diff.cpp
    Utils/json_diff.hpp
    Utils/minilzo_extension.cpp
    Utils/minilzo_extension.hpp
    Utils/MKS.cpp
    Utils/MKS.hpp
    Utils/NetworkAgent.cpp
    Utils/NetworkAgent.hpp
    Utils/Obico.cpp
    Utils/Obico.hpp
    Utils/OctoPrint.cpp
    Utils/OctoPrint.hpp
    Utils/PresetUpdater.cpp
    Utils/PresetUpdater.hpp
    Utils/PrintHost.cpp
    Utils/PrintHost.hpp
    Utils/PrintHostDevices.cpp
    Utils/PrintHostDevices.hpp
    Utils/PrintHostDeviceStatus.cpp
    Utils/PrintHostDeviceStatus.hpp
    Utils/PrusaLinkStatus.cpp
    Utils/PrusaLinkStatus.hpp
    Utils/Process.cpp
    Utils/Process.hpp
    Utils/ProfileDescription.hpp
    Utils/Profile.hpp
    Utils/RaycastManager.cpp
    Utils/RaycastManager.hpp
    Utils/Repetier.cpp
    Utils/Repetier.hpp
    Utils/Serial.cpp
    Utils/Serial.hpp
    Utils/SerialMessage.hpp
    Utils/SerialMessageType.hpp
    Utils/SimplyPrint.cpp
    Utils/SimplyPrint.hpp
    Utils/TCPConsole.cpp
    Utils/TCPConsole.hpp
    Utils/UndoRedo.cpp
    Utils/UndoRedo.hpp
    Utils/WebSocketClient.hpp
    Utils/WxFontUtils.cpp
    Utils/WxFontUtils.hpp
    Utils/MQTT.hpp
    Utils/MQTT.cpp
    Utils/MqttReconnectPolicy.hpp
    Utils/MoonRaker.cpp
    Utils/MoonRaker.hpp
    Utils/TimeSyncManager.cpp
    Utils/TimeSyncManager.hpp
    Utils/TimeoutMap.hpp
)

add_subdirectory(GUI/DeviceTab)

if (WIN32)
    list(APPEND SLIC3R_GUI_SOURCES
            GUI/dark_mode/dark_mode.hpp
            GUI/dark_mode/IatHook.hpp
            GUI/dark_mode/UAHMenuBar.hpp
            GUI/dark_mode.hpp
            GUI/dark_mode.cpp
        )
endif ()

if (APPLE)
    list(APPEND SLIC3R_GUI_SOURCES
            Utils/RetinaHelperImpl.mm
            Utils/MacDarkMode.mm
            GUI/RemovableDriveManagerMM.mm
            GUI/RemovableDriveManagerMM.h
            GUI/Mouse3DHandlerMac.mm
            GUI/InstanceCheckMac.mm
            GUI/InstanceCheckMac.h
            GUI/GUI_UtilsMac.mm
            GUI/wxMediaCtrl2.mm
            GUI/wxMediaCtrl2.h
        )
    FIND_LIBRARY(DISKARBITRATION_LIBRARY DiskArbitration)
else ()
    list(APPEND SLIC3R_GUI_SOURCES
            GUI/wxMediaCtrl2.cpp
            GUI/wxMediaCtrl2.h
        )
endif ()

if (UNIX AND NOT APPLE)
    list(APPEND SLIC3R_GUI_SOURCES
        GUI/Printer/gstbambusrc.c
    )
endif ()

set(ORCA_UPDATER_SIG_KEY_B64 "${ORCA_UPDATER_SIG_KEY}")
string(STRIP "${ORCA_UPDATER_SIG_KEY_B64}" ORCA_UPDATER_SIG_KEY_B64)
string(REPLACE "\n" "" ORCA_UPDATER_SIG_KEY_B64 "${ORCA_UPDATER_SIG_KEY_B64}")
string(REPLACE "\r" "" ORCA_UPDATER_SIG_KEY_B64 "${ORCA_UPDATER_SIG_KEY_B64}")
string(REPLACE "\t" "" ORCA_UPDATER_SIG_KEY_B64 "${ORCA_UPDATER_SIG_KEY_B64}")
string(REPLACE " "  "" ORCA_UPDATER_SIG_KEY_B64 "${ORCA_UPDATER_SIG_KEY_B64}")

set(ORCA_UPDATER_SIG_KEY_AVAILABLE 0)
if(ORCA_UPDATER_SIG_KEY_B64)
    set(ORCA_UPDATER_SIG_KEY_AVAILABLE 1)
endif()

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/GeneratedConfig.hpp.in
               ${CMAKE_CURRENT_BINARY_DIR}/GeneratedConfig.hpp
               @ONLY)

add_library(libslic3r_gui STATIC ${SLIC3R_GUI_SOURCES})
# -DSLIC3R_UNITY_BUILD=ON: these sources do not survive being merged with their batch neighbours
# (duplicate file-local names or leaking macros), so they keep their own translation unit.
slic3r_unity_build(libslic3r_gui SKIP
    GUI/AMSDryCtrlDialog.cpp                      # file-scope using namespace AmsDrying leaks into neighbours
    GUI/AuxiliaryDataViewModel.cpp                # s_default_folders / count clash
    GUI/BindDialog.cpp                            # uses ::CheckBox, a neighbour's Field.hpp brings GUI::CheckBox
    GUI/calib_dlg.cpp                             # file-local names clash
    GUI/CalibrationWizard.cpp                     # NA_STR also in CalibrationWizardCaliPage.cpp
    GUI/ConfigWizard.cpp                          # _MSW_DARK_MODE from a neighbour's GUI_App.hpp pulls a missing header
    GUI/dark_mode.cpp                             # GDI Polygon vs Slic3r::Polygon
    GUI/FcmProvider.cpp                           # T_CONNECT/T_MAX also in ApnsProvider.cpp
    GUI/HostedProvider.cpp                        # T_CONNECT/T_MAX/trim also in ApnsProvider.cpp/AppPush.cpp
    GUI/FilamentGroupDialog.cpp                   # DRAG_PREFIX also in DualNozzleSliceDialog.cpp
    GUI/GUI_ObjectList.cpp                        # printer_technology()/filaments_count() defined by a neighbour too
    GUI/Gizmos/GLGizmoBrimEars.cpp                # HOVER_COLOR clash
    GUI/Gizmos/GLGizmoEmboss.cpp                  # needs IMGUI_DEFINE_MATH_OPERATORS before the first imgui.h
    GUI/Gizmos/GLGizmoMeshBoolean.cpp             # needs IMGUI_DEFINE_MATH_OPERATORS before the first imgui.h
    GUI/Gizmos/GLGizmoSVG.cpp                     # needs IMGUI_DEFINE_MATH_OPERATORS before the first imgui.h
    GUI/GLCanvas3D.cpp                            # needs IMGUI_DEFINE_MATH_OPERATORS before the first imgui.h
    GUI/ImGuiWrapper.cpp                          # needs IMGUI_DEFINE_MATH_OPERATORS before the first imgui.h
    GUI/IMSlider.cpp                              # needs IMGUI_DEFINE_MATH_OPERATORS before the first imgui.h
    GUI/Jobs/SendJob.cpp                          # message strings also defined in PrintJob.cpp
    GUI/MsgDialog.cpp                             # uses ::CheckBox, a neighbour's Field.hpp brings GUI::CheckBox
    GUI/OptionsGroup.cpp                          # titleWidth clash
    GUI/RecenterDialog.cpp                        # #define BORDER leaks into neighbours
    GUI/RemeshDialog.cpp                          # CFG_* constants also in RoundDialog.cpp / FillBedDialog.cpp
    GUI/RoundDialog.cpp                           # CFG_* constants also in RemeshDialog.cpp / FillBedDialog.cpp
    GUI/SelectMachinePop.cpp                      # EVT_* definitions also in SelectMachine.cpp
    GUI/SendToPrinter.cpp                         # file-local names clash with SelectMachine.cpp
    GUI/SliceBakeDialog.cpp                       # CFG_SECTION also in QuadRemeshDialog.cpp
    GUI/SliceInfoPanel.cpp                        # file-local names clash
    GUI/SupportGroupsDialog.cpp                   # uses ::CheckBox, a neighbour's Field.hpp brings GUI::CheckBox
    GUI/TabButton.cpp                             # TAB_BUTTON_* colours clash
    GUI/WipeTowerDialog.cpp                       # #define style leaks into neighbours
    GUI/QuadRemeshDialog.cpp                      # CFG_* constants also in RemeshDialog.cpp / FillBedDialog.cpp
    GUI/ScaleToVolumeDialog.cpp                   # CFG_SECTION / mm_str() also in FillBedDialog.cpp
    GUI/ColorSplitDialog.cpp                      # mm_str() also in FillBedDialog.cpp
    GUI/filamentsync/PlaterPreview.cpp            # g_* style constants also in filamentsync/FilamentColorMapBox.cpp
    GUI/SelectMachine.cpp                         # uses ::CheckBox, a neighbour's Field.hpp brings GUI::CheckBox
    GUI/filamentsync/SyncFilamentColorDialog.cpp  # g_* style constants also in FilamentColorMapBox.cpp
    Utils/FixModelByWin10.cpp                     # a neighbour's using namespace std makes WinRT's byte ambiguous
    GUI/SendMultiMachinePage.cpp                  # uses ::CheckBox, a neighbour's Field.hpp brings GUI::CheckBox
    GUI/DeviceTab/uiDeviceUpdateVersion.cpp       # #define VERSION_STR breaks paho's async_client.h in neighbours
    Utils/Bonjour.cpp                             # optional becomes ambiguous (std vs boost) after a neighbour's using-directive
    GUI/BitmapCache.cpp                           # defines NANOSVG(RAST)_IMPLEMENTATION before its include
    GUI/GLTexture.cpp                             # defines STB_DXT_IMPLEMENTATION before its include
    GUI/IconManager.cpp                           # defines STB_RECT_PACK_IMPLEMENTATION before its include
    GUI/Search.cpp                                # defines FTS_FUZZY_MATCH_IMPLEMENTATION before its include
    GUI/DualNozzleSliceDialog.cpp                 # namespace-scope using namespace DualNozzleSync
    GUI/DualNozzleState.cpp                       # namespace-scope using namespace DualNozzleSync
    GUI/Gizmos/GLGizmoCut.cpp                     # namespace-scope using namespace Geometry
    GUI/Gizmos/GLGizmosCommon.cpp                 # namespace-scope using-directives for the gizmo data objects
    GUI/UntrustedSettingsGuard.cpp                # namespace-scope using namespace Slic3r::untrusted
    GUI/Jobs/CreateFontStyleImagesJob.cpp         # namespace-scope using namespace Emboss
    GUI/Jobs/EmbossJob.cpp                        # namespace-scope using namespace Emboss
    GUI/TextLines.cpp                             # namespace-scope using namespace Emboss
    Utils/EmbossStyleManager.cpp                  # namespace-scope using namespace Emboss
    GUI/WebUserLoginDialog.cpp                    # file-scope using namespace std
    GUI/WebSMUserLoginDialog.cpp                  # file-scope using namespace std
    Utils/json_diff.cpp                           # file-scope using namespace std
    Utils/NetworkAgent.cpp                        # file-scope using namespace BBL
    GUI/GUI_App.cpp                               # includes wx/glcanvas.h (GL/gl.h) before a neighbour's glad
    GUI/Widgets/CheckBox.cpp                      # ::CheckBox vs GUI::CheckBox (Field.hpp) under neighbours' using-directives
    GUI/CalibrationWizardPresetPage.cpp           # uses ::CheckBox, a neighbour's Field.hpp brings GUI::CheckBox
    GUI/CalibrationWizardSavePage.cpp             # uses ::CheckBox, a neighbour's Field.hpp brings GUI::CheckBox
    GUI/CreatePresetsDialog.cpp                   # uses ::CheckBox, a neighbour's Field.hpp brings GUI::CheckBox
    GUI/MultiTaskManagerPage.cpp                  # uses ::CheckBox, a neighbour's Field.hpp brings GUI::CheckBox
    GUI/OG_CustomCtrl.cpp                         # uses ::CheckBox, a neighbour's Field.hpp brings GUI::CheckBox
    GUI/PhysicalPrinterDialog.cpp                 # uses ::CheckBox, a neighbour's Field.hpp brings GUI::CheckBox
    GUI/Plater.cpp                                # uses ::CheckBox, a neighbour's Field.hpp brings GUI::CheckBox
    GUI/PrintHostDialogs.cpp                      # uses ::CheckBox, a neighbour's Field.hpp brings GUI::CheckBox
    GUI/PrintOptionsDialog.cpp                    # uses ::CheckBox, a neighbour's Field.hpp brings GUI::CheckBox
    GUI/ReleaseNote.cpp                           # uses ::CheckBox, a neighbour's Field.hpp brings GUI::CheckBox
    GUI/SMPhysicalPrinterDialog.cpp               # uses ::CheckBox, a neighbour's Field.hpp brings GUI::CheckBox
    GUI/UnsavedChangesDialog.cpp                  # uses ::CheckBox, a neighbour's Field.hpp brings GUI::CheckBox
    GUI/FlashForge/PrintDevLocalFileDlg.cpp       # uses ::CheckBox, a neighbour's Field.hpp brings GUI::CheckBox
    GUI/Selection.cpp                             # CGAL must come before OCCT's Handle() macro (Model.hpp -> STEP.hpp)
    GUI/GLToolbar.cpp                             # includes wx/glcanvas.h (GL/gl.h) before a neighbour's glad
    GUI/GUI_Preview.cpp                           # includes wx/glcanvas.h (GL/gl.h) before a neighbour's glad
    GUI/MainFrame.cpp                             # wx/glcanvas.h before glad; Windows shell headers vs UserNotification
    GUI/Mouse3DController.cpp                     # includes wx/glcanvas.h (GL/gl.h) before a neighbour's glad
    GUI/NotificationManager.cpp                   # includes wx/glcanvas.h (GL/gl.h) before a neighbour's glad
    GUI/OpenGLManager.cpp                         # includes wx/glcanvas.h (GL/gl.h) before a neighbour's glad
    GUI/Gizmos/GLGizmosManager.cpp                # includes wx/glcanvas.h (GL/gl.h) before a neighbour's glad
    GUI/MediaFilePanel.cpp                        # Windows shell headers declare UserNotification (clashes with UserNotification.hpp)
    GUI/wxMediaCtrl2.cpp                          # Windows shell headers declare UserNotification (clashes with UserNotification.hpp)
    GUI/Widgets/WebView.cpp                       # Windows shell headers declare UserNotification (clashes with UserNotification.hpp)
    GUI/AMSSetting.cpp                            # ::CheckBox vs GUI::CheckBox (Field.hpp)
    GUI/Calibration.cpp                           # ::CheckBox vs GUI::CheckBox (Field.hpp)
    GUI/CloneDialog.cpp                           # ::CheckBox vs GUI::CheckBox (Field.hpp)
    GUI/FillBedDialog.cpp                         # ::CheckBox vs GUI::CheckBox (Field.hpp)
    GUI/MediaPlayCtrl.cpp                         # ::CheckBox vs GUI::CheckBox (Field.hpp)
    GUI/ParamsPanel.cpp                           # ::CheckBox vs GUI::CheckBox (Field.hpp)
    GUI/Preferences.cpp                           # ::CheckBox vs GUI::CheckBox (Field.hpp)
    GUI/Tab.cpp                                   # ::CheckBox vs GUI::CheckBox (Field.hpp)
    GUI/AMSMaterialsSetting.cpp                   # ::CheckBox members vs GUI::CheckBox (Field.hpp)
    GUI/AmsMappingPopup.cpp                       # ::CheckBox members vs GUI::CheckBox (Field.hpp)
    GUI/CalibrationWizardPage.cpp                 # ::CheckBox members vs GUI::CheckBox (Field.hpp)
    GUI/ExtrusionCalibration.cpp                  # ::CheckBox members vs GUI::CheckBox (Field.hpp)
)
if (MSVC AND SLIC3R_MSVC_Z7 AND SLIC3R_UNITY_BUILD)
    # A unity build repeats the debug info of the shared headers once per batch instead of once per
    # source file, and /Zc:inline leaves unreferenced inline functions and data out of the objects
    # (the linker's /OPT:REF drops them from the binaries anyway). Together they keep this /Z7
    # library under the 4 GB static-library limit, so it stays cacheable: 4.3 GB without /Zc:inline,
    # 3.9 GB with it at 8 sources per batch, 3.4 GB at -DSLIC3R_UNITY_BUILD_BATCH_SIZE=16 (use that).
    target_compile_options(libslic3r_gui PRIVATE /Zc:inline)
elseif (MSVC AND SLIC3R_MSVC_Z7)
    # With /Z7 the debug info embedded in this library pushes it past the 4 GB static-library limit
    # (LNK1248), so this one target keeps /Zi and the real compiler (bypassing any CLToolExe cache wrapper).
    target_compile_options(libslic3r_gui PRIVATE /Zi)
    get_filename_component(_slic3r_gui_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_property(TARGET libslic3r_gui PROPERTY VS_GLOBAL_CLToolExe "cl.exe")
    set_property(TARGET libslic3r_gui PROPERTY VS_GLOBAL_CLToolPath "${_slic3r_gui_cl_dir}")
endif ()
target_include_directories(libslic3r_gui PRIVATE Utils ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(libslic3r_gui paho-mqttpp3-static)

target_include_directories(libslic3r_gui PRIVATE ${PAHO_MQTT_CPP_PATH}/include)

target_include_directories(libslic3r_gui PRIVATE ${PAHO_MQTT_C_PATH}/src)

if (WIN32)
    target_include_directories(libslic3r_gui SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../deps/WebView2/include)
    target_link_libraries(libslic3r_gui Advapi32)
endif()

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SLIC3R_GUI_SOURCES})

encoding_check(libslic3r_gui)

find_package(libnoise REQUIRED)
target_link_libraries(libslic3r_gui libslic3r cereal::cereal imgui imguizmo minilzo glad OpenGL::GL hidapi ${wxWidgets_LIBRARIES} glfw libcurl OpenSSL::SSL OpenSSL::Crypto noise::noise)

if (MSVC)
    target_link_libraries(libslic3r_gui Setupapi.lib)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    FIND_LIBRARY(WAYLAND_SERVER_LIBRARIES NAMES wayland-server)
    FIND_LIBRARY(WAYLAND_EGL_LIBRARIES    NAMES wayland-egl)
    FIND_LIBRARY(WAYLAND_CLIENT_LIBRARIES NAMES wayland-client)
    find_package(CURL REQUIRED)
    target_link_libraries(libslic3r_gui
        OpenGL::EGL
        ${DBUS_LIBRARIES}
        ${WAYLAND_SERVER_LIBRARIES}
        ${WAYLAND_EGL_LIBRARIES}
        ${WAYLAND_CLIENT_LIBRARIES}
        ${CURL_LIBRARIES}
    )
elseif (APPLE)
    target_link_libraries(libslic3r_gui ${DISKARBITRATION_LIBRARY} "-framework Security")
endif()

if (SLIC3R_STATIC)
    # FIXME: This was previously exported by wx-config but the wxWidgets
    # cmake build forgets this and the build fails in debug mode (or on raspberry release)
    target_compile_definitions(libslic3r_gui PUBLIC -DwxDEBUG_LEVEL=0)
endif()

if (SPNAV_LIB)
    target_link_libraries(libslic3r_gui ${SPNAV_LIB})
endif()

if (SLIC3R_STATIC AND NOT SLIC3R_STATIC_EXCLUDE_CURL AND UNIX AND NOT APPLE)
    target_compile_definitions(libslic3r_gui PRIVATE OPENSSL_CERT_OVERRIDE)
endif ()

if (SLIC3R_PCH AND NOT SLIC3R_SYNTAXONLY)
    add_precompiled_header(libslic3r_gui pchheader.hpp FORCEINCLUDE)
elseif (SLIC3R_UNITY_BUILD AND MSVC)
    # A unity build without the PCH (a compiler cache cannot cache /Yc /Yu compiles) still starts
    # every translation unit with pchheader.hpp, so the batches see the same Windows macros and
    # include order as the PCH build instead of whatever their first source happened to include.
    target_compile_options(libslic3r_gui PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/FI${CMAKE_CURRENT_SOURCE_DIR}/pchheader.hpp>")
endif ()

# We need to implement some hacks for wxWidgets and touch the underlying GTK
# layer and sub-libraries. This forces us to use the include locations and
# link these libraries.
if (UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    find_package(GTK${SLIC3R_GTK} REQUIRED)
    pkg_check_modules(LIBSECRET REQUIRED libsecret-1)
    if (FLATPAK)
        # I don't know why this is needed, but for whatever reason slic3r isn't
        # linking to X11 and webkit2gtk. force it.
        find_package(X11 REQUIRED)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(webkit2gtk REQUIRED webkit2gtk-4.1)
        target_link_libraries (libslic3r_gui ${X11_LIBRARIES} ${webkit2gtk_LIBRARIES})
    endif()
    target_include_directories(libslic3r_gui SYSTEM PRIVATE ${GTK${SLIC3R_GTK}_INCLUDE_DIRS} ${LIBSECRET_INCLUDE_DIRS})
    target_link_libraries(libslic3r_gui ${GTK${SLIC3R_GTK}_LIBRARIES} fontconfig ${LIBSECRET_LIBRARIES})


    # We add GStreamer for bambu:/// support.
    pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0)
    pkg_check_modules(GST_BASE REQUIRED gstreamer-base-1.0)
    target_link_libraries(libslic3r_gui ${GSTREAMER_LIBRARIES} ${GST_BASE_LIBRARIES})
    target_include_directories(libslic3r_gui SYSTEM PRIVATE ${GSTREAMER_INCLUDE_DIRS} ${GST_BASE_INCLUDE_DIRS})
endif ()

# Add a definition so that we can tell we are compiling slic3r.
target_compile_definitions(libslic3r_gui PRIVATE SLIC3R_CURRENTLY_COMPILING_GUI_MODULE)

# Sentry crash reporting integration
if (SLIC3R_SENTRY)
    target_compile_definitions(libslic3r_gui PUBLIC SLIC3R_SENTRY)
    target_link_libraries(libslic3r_gui sentry::sentry)
endif()