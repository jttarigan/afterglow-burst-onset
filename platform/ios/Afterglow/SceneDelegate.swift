import UIKit

final class SceneDelegate: UIResponder, UIWindowSceneDelegate {
    var window: UIWindow?
    private var game: GameViewController?

    func scene(_ scene: UIScene, willConnectTo session: UISceneSession,
               options connectionOptions: UIScene.ConnectionOptions) {
        guard let windowScene = scene as? UIWindowScene else { return }
        let window = UIWindow(windowScene: windowScene)
        let vc = GameViewController()
        window.rootViewController = vc
        window.makeKeyAndVisible()
        self.window = window
        self.game = vc
    }

    // A backgrounded or interrupted run is invalid (docs/04 §2 rule 2): end it and say so.
    func sceneWillResignActive(_ scene: UIScene) { game?.hostEvent("interrupt") }
    func sceneDidEnterBackground(_ scene: UIScene) { game?.hostEvent("background") }
}
