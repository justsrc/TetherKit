import XCTest
import TetherKitIPC
@testable import TetherKitUIModel

/// Locks the activation-turn write escape's commit semantics — the
/// mechanics the log-level picker (and every future routed control
/// binding) rides on.
///
/// What is asserted is the machine-provable half of the round 8
/// verdict (AGENTS.md pitfall 24): "`.menu` picker is the canonical
/// keyboard-operable replacement":
/// the value that a picker's selection binding commits must land
/// synchronously in the model outside a user turn, and the direct
/// accessibility turn must never hop at all (pitfall 24). The other
/// half — that a real keyboard can open the popped menu and commit an
/// item — is not synthesizable headlessly (menu tracking runs in
/// NSEventTrackingRunLoopMode beyond every in-process event channel)
/// and is covered by the hardware checklist that ships with this
/// round's probe evidence.
final class ButtonTurnTests: XCTestCase {

    /// Stand-in for AppModel's `logLevelFilter` slot: the model side of
    /// the binding under test.
    private final class FilterModel {
        var level: LogLevel = .info
    }

    // MARK: Routed binding commit

    func testRoutedBindingCommitLandsInModel() {
        let model = FilterModel()
        let binding = ButtonTurn.routed(
            get: { model.level },
            set: { model.level = $0 })

        // A unit test runs outside any activation turn
        // (NSApp.currentEvent == nil), so the routed write is direct
        // and synchronous — exactly the condition the audit path
        // relies on.
        binding.wrappedValue = .warning
        XCTAssertEqual(model.level, .warning)

        binding.wrappedValue = .error
        XCTAssertEqual(model.level, .error)
    }

    func testRoutedBindingGetReadsModel() {
        let model = FilterModel()
        model.level = .trace
        let binding = ButtonTurn.routed(
            get: { model.level },
            set: { model.level = $0 })
        XCTAssertEqual(binding.wrappedValue, .trace)
    }

    override func tearDown() {
        super.tearDown()
        ButtonTurn.currentEventTypeProvider = { NSApp?.currentEvent?.type }
    }

    // MARK: User turn hop vs direct execution

    func testWriteInUserTurnHopsToMainQueue() {
        let model = FilterModel()
        ButtonTurn.currentEventTypeProvider = { .leftMouseDown }

        let expectation = expectation(description: "hopped write lands on main queue")
        ButtonTurn.write {
            model.level = .warning
            expectation.fulfill()
        }

        // Must not have run synchronously:
        XCTAssertEqual(model.level, .info, "write must hop rather than land synchronously during user event turn")
        wait(for: [expectation], timeout: 2.0)
        XCTAssertEqual(model.level, .warning)
    }

    func testWriteDirectInUserTurnBypassesHop() {
        let model = FilterModel()
        ButtonTurn.currentEventTypeProvider = { .leftMouseDown }

        // writeDirect must land synchronously even inside an event turn (AXPress turn):
        ButtonTurn.writeDirect {
            model.level = .error
        }
        XCTAssertEqual(model.level, .error, "writeDirect must land synchronously without hopping")
    }

    func testWriteOutsideUserTurnLandsSynchronously() {
        let model = FilterModel()
        ButtonTurn.currentEventTypeProvider = { nil }

        ButtonTurn.write {
            model.level = .debug
        }
        XCTAssertEqual(model.level, .debug, "write outside user turn must land synchronously")
    }

    // MARK: Direct accessibility-turn write

    func testDirectWriteLandsSynchronously() {
        // Pitfall 24: a turn scheduled into an accessibility press is
        // dropped before running, so the direct entry point must write
        // in the calling turn — asserted here as synchronous landing.
        let model = FilterModel()
        ButtonTurn.writeDirect { model.level = .error }
        XCTAssertEqual(model.level, .error)
    }
}
