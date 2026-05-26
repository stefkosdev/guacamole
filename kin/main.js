(function() {
    function qp(n) {
        try { return new URLSearchParams(location.search).get(n) || ''; } catch(_e) { return ''; }
    }
    function run() {
        if (!window.kin || !kin.classes || !kin.classes.Window) {
            console.error('kin_guacamole_admin: kin app API unavailable');
            return;
        }
        new kin.classes.Window({
            entry: 'app.js',
            packageId: qp('kin_repo_package') || 'kin_guacamole_admin',
            title: 'Guacamole remote desktop manager',
            width: 1000,
            height: 700,
            quitOnClose: true,
            module: true,
            assets: [
                { type: 'css', href: '../kin_ui/theme/kin-ui.css' },
                { type: 'css', href: 'guacamole-view.css' }
            ]
        });
    }
    run();
})();
