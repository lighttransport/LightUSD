#!/usr/bin/env python3
"""Explicit development bootstrap for testing a fork bridge with an installed USD.

Production sessions use usdview's built-in --remote-control switch instead.
"""
import importlib.util
from pathlib import Path
import signal
import sys
from pxr import Usdviewq


class Launcher(Usdviewq.Launcher):
    def RegisterOptions(self, parser):
        super().RegisterOptions(parser)
        parser.add_argument('--bridge-source', required=True, type=Path)
        parser.add_argument('--remote-control', action='store_true')
        parser.add_argument('--remote-control-port', default=0, type=int)
        parser.add_argument('--remote-token-file', required=True, type=Path)
        parser.add_argument('--remote-info-file', required=True, type=Path)

    def LaunchPreamble(self, args):
        app, controller = super().LaunchPreamble(args)
        view = controller._stageView
        if not hasattr(view, 'signalRemoteFrameRendered'):
            if not hasattr(view, 'frameSwapped'):
                raise RuntimeError('installed Qt viewer lacks completed-frame notification')
            view.signalRemoteFrameRendered = view.frameSwapped
        Usdviewq.__path__.append(str(args.bridge_source.resolve().parent))
        name = Usdviewq.__name__ + '.remoteControl'
        spec = importlib.util.spec_from_file_location(name, args.bridge_source)
        module = importlib.util.module_from_spec(spec)
        sys.modules[name] = module
        spec.loader.exec_module(module)
        controller._remoteControl = module.RemoteControl(
            controller, args.remote_control_port,
            args.remote_token_file.read_text().strip(), args.remote_info_file)
        return app, controller


if __name__ == '__main__':
    signal.signal(signal.SIGINT, signal.SIG_DFL)
    Launcher().Run()
