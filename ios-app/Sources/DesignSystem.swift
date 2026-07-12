import SwiftUI

/// Shared visual tokens for the app, implementing docs/UI_DESIGN.md.
/// Keep these values in sync with that document and the web panel CSS.
enum Theme {
    // Accent
    static let accent = Color(hex: 0x3D7BFF)
    static let accentPressed = Color(hex: 0x2E63E6)

    // Status palette (mirrors iOS system semantic colours)
    static let idleGrey = Color(hex: 0x8E8E93)
    static let connectAmber = Color(hex: 0xFF9F0A)
    static let liveGreen = Color(hex: 0x30D158)
    static let errorRed = Color(hex: 0xFF453A)

    // Over-video surfaces
    static let glassPanel = Color.black.opacity(0.55)
    static let glassChip = Color.white.opacity(0.12)
    static func glassChipOn() -> Color { accent.opacity(0.9) }

    static let textPrimary = Color.white
    static let textSecondary = Color.white.opacity(0.6)

    // Spacing scale
    enum Space {
        static let xs: CGFloat = 4
        static let s: CGFloat = 8
        static let m: CGFloat = 12
        static let l: CGFloat = 16
        static let xl: CGFloat = 20
        static let xxl: CGFloat = 24
    }

    // Radius
    enum Radius {
        static let panel: CGFloat = 16
        static let chip: CGFloat = 12
    }

    static let controlButton: CGFloat = 44
    static let iconSize: CGFloat = 22
}

extension Color {
    init(hex: UInt32) {
        self.init(
            .sRGB,
            red: Double((hex >> 16) & 0xFF) / 255,
            green: Double((hex >> 8) & 0xFF) / 255,
            blue: Double(hex & 0xFF) / 255,
            opacity: 1)
    }
}

/// A circular 44 pt glass control button (icon only). `active` fills it
/// with the accent colour (e.g. flashlight on).
struct ControlButton: View {
    let systemImage: String
    var active = false
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Image(systemName: systemImage)
                .font(.system(size: 18, weight: .medium))
                .foregroundColor(Theme.textPrimary)
                .frame(width: Theme.controlButton, height: Theme.controlButton)
                .background(active ? Theme.glassChipOn() : Theme.glassChip,
                            in: Circle())
        }
    }
}

extension View {
    /// Floating control panel: padding + translucent material + radius.
    func glassPanel() -> some View {
        self
            .padding(Theme.Space.l)
            .background(Theme.glassPanel, in:
                RoundedRectangle(cornerRadius: Theme.Radius.panel))
    }

    /// Small pill used for the status chip.
    func glassPill() -> some View {
        self
            .padding(.horizontal, Theme.Space.m)
            .padding(.vertical, Theme.Space.s)
            .background(Theme.glassPanel, in: Capsule())
    }
}
