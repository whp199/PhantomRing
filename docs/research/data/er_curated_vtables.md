| class | TD RVA | COL RVA | COL off | vtable RVA | #vf | vfunc sections | direct bases (first 4) |
|---|---|---|---|---|---|---|---|
| `CS::CSCam` | 0x3C7F500 | 0x32F0298 | 0x0 | 0x2AA0A18 | 4 | .text#0:4 |  |
| `CS::CSCameraImp` | 0x3C7F710 | 0x32CEBC0 | 0x0 | 0x2A2AFF0 | 1 | .text#0:1 |  |
| `CS::CSCameraStep` | 0x3C7F7A8 | 0x32CECB8 | 0x0 | 0x2A2B798 | 19 | .text#0:19 | CS::CSStepTask<class CS::CSCameraStep>, FD4::FD4StepTaskBase<class CS::CSCameraStep>, FD4::FD4StepTemplateBase<class CS::CSCameraStep, class FD4::FD4TaskBase>, FD4::FD4StepTemplateInterface<class FD4::FD4TaskBase> |
| `CS::CSPersCam` | 0x3C7F4D8 | 0x32F01F8 | 0x0 | 0x2AA0A40 | 4 | .text#0:4 | CS::CSCam |
| `CS::ChrCam` | 0x3C7F568 | 0x32CE840 | 0x0 | 0x2A2AA08 | 4 | .text#0:4 | CS::CSPersCam, CS::CSCam |
| `CS::ChrExFollowCam` | 0x3C7F698 | 0x32CEAB8 | 0x0 | 0x2A2ACA0 | 4 | .text#0:4 | CS::CSPersCam, CS::CSCam |
| `CS::ChrAimCam` | 0x3C7F4B0 | 0x32CE690 | 0x0 | 0x2A2A7F0 | 4 | .text#0:4 | CS::CSPersCam, CS::CSCam |
| `CS::CSDebugCam` | 0x3CF3A10 | 0x3343818 | 0x0 | 0x2B672C0 | 4 | .text#0:4 | CS::CSPersCam, CS::CSCam |
| `CS::CSDistViewCam` | 0x3C7FA38 | 0x32CF128 | 0x0 | 0x2A2BAD0 | 4 | .text#0:4 | CS::CSPersCam, CS::CSCam |
| `CS::WorldChrManImp` | 0x3C860A8 | 0x32D9A88 | 0x0 | 0x2A4E980 | 1 | .text#0:1 |  |
| `CS::WorldChrManDbgImp` | 0x3C862F8 | 0x32D9EB8 | 0x0 | 0x2A4EC48 | 1 | .text#0:1 |  |
| `CS::PlayerIns` | 0x3C85DD8 | 0x32E7DF0 | 0x0 | 0x2A7FBB0 | 204 | .text#0:204 | CS::ChrIns, CS::FieldInsBase |
| `CS::ChrIns` | 0x3C80368 | 0x32D03A0 | 0x0 | 0x2A310C8 | 203 | .text#0:203 | CS::FieldInsBase |
| `CS::EnemyIns` | 0x3C84E60 | 0x32D7D90 | 0x0 | 0x2A47090 | 203 | .text#0:203 | CS::ChrIns, CS::FieldInsBase |
| `CS::FieldInsBase` | 0x3C7E240 | 0x32CD448 | 0x0 | 0x2A28E78 | 8 | .text#0:8 |  |
| `CS::CSTaskImp` | 0x3D10340 | 0x3374820 | 0x0 | 0x2C04B00 | 1 | .text#0:1 |  |
| `CS::CSTaskGroupIns` | 0x3D10490 | 0x3374A98 | 0x0 | 0x2C08CF8 | 3 | .text#0:3 |  |
| `CS::CSEzTask` | 0x3C67778 | 0x3374628 | 0x0 | 0x2C048A0 | 6 | .text#0:6 | FD4::FD4TaskBase, FD4::FD4ComponentBase |
| `CS::CSEzTaskProxy` | 0x3D102A8 | 0x33746C8 | 0x0 | 0x2C049F8 | 4 | .text#0:4 | FD4::FD4TaskBase, FD4::FD4ComponentBase |
| `CS::CSEzRabbitTask` | 0x3C86178 | 0x3374678 | 0x0 | 0x2C04910 | 6 | .text#0:6 | CS::CSEzRabbitTaskBase, CS::CSEzTask, FD4::FD4TaskBase, FD4::FD4ComponentBase |
| `CS::CSPad` | 0x3D0BC40 | 0x336D7A0 | 0x0 | 0x2BE3A70 | 7 | .text#0:7 | FD4::FD4BasePad |
| `CS::CSPadStep` | 0x3D0BF78 | 0x336D848 | 0x0 | 0x2BE4378 | 19 | .text#0:19 | CS::CSStepTask<class CS::CSPadStep>, FD4::FD4StepTaskBase<class CS::CSPadStep>, FD4::FD4StepTemplateBase<class CS::CSPadStep, class FD4::FD4TaskBase>, FD4::FD4StepTemplateInterface<class FD4::FD4TaskBase> |
| `CS::CSInGamePad` | 0x3C7F588 | 0x32CE8C8 | 0x0 | 0x2A2AA40 | 1 | .text#0:1 | CS::CSPadWapper<class CS::CSInGamePad_UserInput2, class CS::CSInGamePad_UserInput1> |
| `CS::CSInGamePad_UserInput1` | 0x3D0BEB0 | 0x336D5E0 | 0x0 | 0x2BE35E8 | 7 | .text#0:7 | CS::CSPad, FD4::FD4BasePad |
| `CS::PadManipulator` | 0x3C7FFD0 | 0x32CFB18 | 0x0 | 0x2A2E788 | 55 | .text#0:55 | CS::ChrManipulator |
| `DLUID::PadDevice<class DLKR::DLMultiThreadingPolicy>` | 0x3D4BBD0 | 0x33DECD0 | 0x0 | 0x30CD048 | 51 | .text#0:51 | DLUID::DLUserInputDeviceImpl<class DLKR::DLMultiThreadingPolicy>, DLUI::DLUserInputDevice<class DLKR::DLMultiThreadingPolicy> |
| `DLUID::KeyboardDevice<class DLKR::DLMultiThreadingPolicy>` | 0x3D4BC20 | 0x33DED58 | 0x0 | 0x30CD4D0 | 51 | .text#0:51 | DLUID::DLUserInputDeviceImpl<class DLKR::DLMultiThreadingPolicy>, DLUI::DLUserInputDevice<class DLKR::DLMultiThreadingPolicy> |
| `DLUID::MouseDevice<class DLKR::DLMultiThreadingPolicy>` | 0x3D4BC70 | 0x33DEDE0 | 0x0 | 0x30CD840 | 51 | .text#0:51 | DLUID::DLUserInputDeviceImpl<class DLKR::DLMultiThreadingPolicy>, DLUI::DLUserInputDevice<class DLKR::DLMultiThreadingPolicy> |
| `DLUID::VirtualMultiDevice<class DLKR::DLMultiThreadingPolicy>` | 0x3D4BCC0 | 0x33DEE68 | 0x0 | 0x30CDA30 | 51 | .text#0:51 | DLUID::DLUserInputDeviceImpl<class DLKR::DLMultiThreadingPolicy>, DLUI::DLUserInputDevice<class DLKR::DLMultiThreadingPolicy> |
| `DLUID::DLUserInputManagerImpl<class DLKR::DLMultiThreadingPolicy>` | 0x3D48290 | 0x33D9A18 | 0x0 | 0x30BC908 | 5 | .text#0:5 | DLUID::DLUserInputManagerImplBase<class DLKR::DLMultiThreadingPolicy> |
| `DLUID::DLUserInputDeviceImpl<class DLKR::DLMultiThreadingPolicy>` | 0x3D4BB20 | 0x33E1090 | 0x0 | 0x30D79E0 | 51 | .text#0:51 | DLUI::DLUserInputDevice<class DLKR::DLMultiThreadingPolicy> |
| `CS::CSGraphicsImp` | 0x3CF3E50 | 0x3343FF8 | 0x0 | 0x2B68558 | 1 | .text#0:1 |  |
| `CS::CSGraphicsStep` | 0x3CF3F60 | 0x3344170 | 0x0 | 0x2B69BE0 | 19 | .text#0:19 | CS::CSStepTask<class CS::CSGraphicsStep>, FD4::FD4StepTaskBase<class CS::CSGraphicsStep>, FD4::FD4StepTemplateBase<class CS::CSGraphicsStep, class FD4::FD4TaskBase>, FD4::FD4StepTemplateInterface<class FD4::FD4TaskBase> |
| `CS::CSDrawStep` | 0x3CF3B40 | 0x3343A90 | 0x0 | 0x2B67DD0 | 28 | .text#0:28 | CS::CSStepTask<class CS::CSDrawStep>, FD4::FD4StepTaskBase<class CS::CSDrawStep>, FD4::FD4StepTemplateBase<class CS::CSDrawStep, class FD4::FD4TaskBase>, FD4::FD4StepTemplateInterface<class FD4::FD4TaskBase> |
| `CS::CSFlverDrawSystem` | 0x3CF58D8 | 0x3346F50 | 0x0 | 0x2B71800 | 25 | .text#0:25 | GXSR::GXFlverDrawSystem |
| `CS::CSEzDraw` | 0x3CFA358 | 0x334EAE8 | 0x0 | 0x2B814C0 | 6 | .text#0:6 | FD4::FD4HkEzDraw, FD4::FD4EzDraw |
| `CS::CSSgEzDraw` | 0x3CFA3A8 | 0x334EBE8 | 0x0 | 0x2B81560 | 7 | .text#0:7 | GXSG::GXSgEntity, GXSG::GXSgDrawable, GXSG::GXSgObject, GXSG::GXSgTraverserAcceptor |
| `CS::CSSgEzDraw` | 0x3CFA3A8 | 0x334ECB0 | 0x10 | 0x2B815A0 | 1 | .text#0:1 | GXSG::GXSgEntity, GXSG::GXSgDrawable, GXSG::GXSgObject, GXSG::GXSgTraverserAcceptor |
| `CS::CSSgEzDraw` | 0x3CFA3A8 | 0x334ECD8 | 0x80 | 0x2B815B0 | 2 | .text#0:2 | GXSG::GXSgEntity, GXSG::GXSgDrawable, GXSG::GXSgObject, GXSG::GXSgTraverserAcceptor |
| `FD4::FD4HkEzDraw` | 0x3CFA308 | 0x334EA18 | 0x0 | 0x2B81488 | 6 | .text#0:6 | FD4::FD4EzDraw |
| `CS::CSMenuManImp` | 0x3C968A0 | 0x32EFD48 | 0x0 | 0x2A9EED8 | 1 | .text#0:1 |  |
| `CS::CSFeManImp` | 0x3C96AC8 | 0x32F0220 | 0x0 | 0x2AA0A08 | 1 | .text#0:1 |  |
| `CS::CSScaleformStep` | 0x3D04BF0 | 0x3362200 | 0x0 | 0x2BBD870 | 19 | .text#0:19 | CS::CSStepTask<class CS::CSScaleformStep>, FD4::FD4StepTaskBase<class CS::CSScaleformStep>, FD4::FD4StepTemplateBase<class CS::CSScaleformStep, class FD4::FD4TaskBase>, FD4::FD4StepTemplateInterface<class FD4::FD4TaskBase> |
| `CS::CSWindowImp` | 0x3D0F540 | 0x3373490 | 0x0 | 0x2C011E8 | 3 | .text#0:3 | FD4::FD4Window |
| `CS::CSFlipperImp` | 0x3D0EF50 | 0x3372B08 | 0x0 | 0x2BFF358 | 1 | .text#0:1 |  |
| `CS::GameMan` | 0x3C8FD60 | 0x32E8778 | 0x0 | 0x2A82110 | 1 | .text#0:1 |  |
| `CS::CSChrPhysicsModule` | 0x3C83458 | 0x32D4270 | 0x0 | 0x2A3C890 | 2 | .text#0:2 | CS::CSChrModuleBase, FD4::FD4ComponentBase |
| `CS::CSChrBehaviorModule` | 0x3C80950 | 0x32D0ED0 | 0x0 | 0x2A35D48 | 2 | .text#0:2 | CS::CSChrModuleBase, FD4::FD4ComponentBase |
| `CS::CSChrTimeActModule` | 0x3C824B0 | 0x32D22A0 | 0x0 | 0x2A37670 | 2 | .text#0:2 | CS::CSChrModuleBase, FD4::FD4ComponentBase |
| `CS::CSChrActionRequestModule` | 0x3C80670 | 0x32D09D8 | 0x0 | 0x2A32780 | 2 | .text#0:2 | CS::CSChrModuleBase, FD4::FD4ComponentBase |
| `CS::CSHavokManImp` | 0x3CFD8C8 | 0x3355640 | 0x0 | 0x2B934E8 | 1 | .text#0:1 |  |
| `CS::CSSessionManagerImp` | 0x3CFF778 | 0x3358E08 | 0x0 | 0x2B9D1B8 | 2 | .text#0:2 | CS::CSNRManagerListener |
| `CS::CSNetManImp` | 0x3C67700 | 0x32AF338 | 0x0 | 0x29CDEB8 | 1 | .text#0:1 |  |
| `CS::CSLuaEventProxy` | 0x3C8A610 | 0x32E1258 | 0x0 | 0x2A63608 | 1 | .text#0:1 |  |
| `CS::CSRemoMan` | 0x3CEF038 | 0x333D200 | 0x0 | 0x2B55EE0 | 25 | .text#0:25 | CS::CSStepLocal<class CS::CSRemoMan>, FD4::FD4StepBase<class CS::CSRemoMan>, FD4::FD4StepTemplateBase<class CS::CSRemoMan, class FD4::FD4StepBaseInterface>, FD4::FD4StepTemplateInterface<class FD4::FD4StepBaseInterface> |
| `CS::CSGaitemImp` | 0x3C8FB20 | 0x32E8240 | 0x0 | 0x2A814A0 | 1 | .text#0:1 |  |
| `CS::CSSfxImp` | 0x3D05AE0 | 0x33640E0 | 0x0 | 0x2BC31A0 | 1 | .text#0:1 |  |
