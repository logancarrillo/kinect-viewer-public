import AVFoundation
import SwiftUI
import UIKit

/// Metadata-only QR code scanner (`AVCaptureMetadataOutput` for `.qr`).
/// Never attaches `AVCaptureVideoDataOutput` or `AVCapturePhotoOutput` and never writes to disk.
struct QRScannerView: UIViewControllerRepresentable {
    let onQRCodeScanned: (String) -> Void

    func makeUIViewController(context: Context) -> QRScannerViewController {
        let vc = QRScannerViewController()
        vc.onCodeScanned = onQRCodeScanned
        return vc
    }

    func updateUIViewController(_ uiViewController: QRScannerViewController, context: Context) {}
}

private final class SendableSessionBox: @unchecked Sendable {
    let session = AVCaptureSession()
}

final class QRScannerViewController: UIViewController, @preconcurrency AVCaptureMetadataOutputObjectsDelegate {
    var onCodeScanned: ((String) -> Void)?

    private let sessionBox = SendableSessionBox()
    private var session: AVCaptureSession { sessionBox.session }
    private let sessionQueue = DispatchQueue(label: "kinectviewer.pocket.qr")
    private var previewLayer: AVCaptureVideoPreviewLayer?
    private let statusLabel = UILabel()
    private var didEmit = false

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = UIColor.secondarySystemBackground
        view.layer.cornerRadius = 14
        view.clipsToBounds = true

        statusLabel.font = UIFont.preferredFont(forTextStyle: .footnote)
        statusLabel.textColor = .secondaryLabel
        statusLabel.numberOfLines = 0
        statusLabel.textAlignment = .center
        statusLabel.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(statusLabel)
        NSLayoutConstraint.activate([
            statusLabel.leadingAnchor.constraint(equalTo: view.leadingAnchor, constant: 16),
            statusLabel.trailingAnchor.constraint(equalTo: view.trailingAnchor, constant: -16),
            statusLabel.centerYAnchor.constraint(equalTo: view.centerYAnchor)
        ])

        configureCameraAuthorization()
    }

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        previewLayer?.frame = view.bounds
    }

    override func viewWillDisappear(_ animated: Bool) {
        super.viewWillDisappear(animated)
        let box = sessionBox
        sessionQueue.async {
            if box.session.isRunning {
                box.session.stopRunning()
            }
        }
    }

    private func configureCameraAuthorization() {
        switch AVCaptureDevice.authorizationStatus(for: .video) {
        case .authorized:
            setupCaptureSession()
        case .notDetermined:
            AVCaptureDevice.requestAccess(for: .video) { [weak self] granted in
                DispatchQueue.main.async {
                    if granted {
                        self?.setupCaptureSession()
                    } else {
                        self?.statusLabel.text = "Camera access denied. Paste an invite link or enter the 8-digit pairing code below."
                    }
                }
            }
        default:
            statusLabel.text = "Camera unavailable. Paste an invite link or enter the 8-digit pairing code below."
        }
    }

    private func setupCaptureSession() {
        guard let videoDevice = AVCaptureDevice.default(for: .video),
              let deviceInput = try? AVCaptureDeviceInput(device: videoDevice),
              session.canAddInput(deviceInput) else {
            statusLabel.text = "No camera hardware available (e.g. Simulator). Paste an invite link or enter the 8-digit code below."
            return
        }

        session.beginConfiguration()
        session.addInput(deviceInput)

        let metadataOutput = AVCaptureMetadataOutput()
        if session.canAddOutput(metadataOutput) {
            session.addOutput(metadataOutput)
            metadataOutput.setMetadataObjectsDelegate(self, queue: DispatchQueue.main)
            if metadataOutput.availableMetadataObjectTypes.contains(.qr) {
                metadataOutput.metadataObjectTypes = [.qr]
            }
        }
        session.commitConfiguration()

        let layer = AVCaptureVideoPreviewLayer(session: session)
        layer.videoGravity = .resizeAspectFill
        layer.frame = view.bounds
        view.layer.insertSublayer(layer, at: 0)
        self.previewLayer = layer
        statusLabel.isHidden = true

        let box = sessionBox
        sessionQueue.async {
            box.session.startRunning()
        }
    }

    func metadataOutput(
        _ output: AVCaptureMetadataOutput,
        didOutput metadataObjects: [AVMetadataObject],
        from connection: AVCaptureConnection
    ) {
        guard !didEmit else { return }
        for obj in metadataObjects {
            if let readable = obj as? AVMetadataMachineReadableCodeObject,
               readable.type == .qr,
               let str = readable.stringValue,
               ParsedCameraLink.isSupportedQRURI(str) {
                didEmit = true
                let box = sessionBox
                sessionQueue.async {
                    box.session.stopRunning()
                }
                onCodeScanned?(str)
                break
            }
        }
    }
}
