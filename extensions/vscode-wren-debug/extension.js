const vscode = require('vscode');

// The Wren VM (or the application embedding it) runs its own DAP server on a
// local TCP port; this extension just points VS Code at that port. There is
// no separate adapter process to launch.
class WrenDebugAdapterServerFactory {
    createDebugAdapterDescriptor(session) {
        const port = session.configuration.port || 4711;
        const host = session.configuration.host || '127.0.0.1';
        return new vscode.DebugAdapterServer(port, host);
    }
}

function activate(context) {
    context.subscriptions.push(
        vscode.debug.registerDebugConfigurationProvider('wren', {
            resolveDebugConfiguration(folder, config) {
                if (!config.type && !config.request && !config.name) {
                    // Launched with no configuration: attach to the default
                    // port.
                    return {
                        type: 'wren',
                        request: 'attach',
                        name: 'Attach to Wren VM',
                        port: 4711,
                    };
                }
                if (!config.port) {
                    config.port = 4711;
                }
                return config;
            },
        }));

    context.subscriptions.push(
        vscode.debug.registerDebugAdapterDescriptorFactory(
            'wren', new WrenDebugAdapterServerFactory()));
}

function deactivate() {}

module.exports = { activate, deactivate };
