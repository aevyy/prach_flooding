import os
from worker_thread import WorkerThread


class ra_spoof(WorkerThread):
    def start(self):
        self.config.image_name = "ghcr.io/oran-testing/ra-spoof"
        self.cleanup_old_containers()
        self.setup_env()
        self.setup_networks()

        self.config.container_volumes[self.config.config_file] = {
            "bind": "/etc/ra-spoof/config.yaml",
            "mode": "ro"
        }
        self.config.container_env["RA_SPOOF_CONFIG"] = "/etc/ra-spoof/config.yaml"

        self.setup_volumes()
        self.start_container()
