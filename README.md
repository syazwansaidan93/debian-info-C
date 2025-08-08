# C System Monitor API

This project is a lightweight C-based API designed to monitor the system status of an embedded device, such as the Orange Pi Zero 3. It provides real-time system statistics over an HTTP connection, making it ideal for web-based dashboards or remote monitoring applications.

The server uses a **thread pool** model to efficiently handle multiple, frequent client requests (e.g., a web client polling for updates every second) without the high overhead of creating a new thread for each request.

## Features

* **CPU Usage**: Percentage of CPU in use.

* **System Uptime**: Total time the system has been running in seconds.

* **RAM and SWAP**: Total and used memory in kilobytes (KB).

* **CPU Temperature**: Raw temperature in millicelsius (mC).

* **Disk Usage**: Total and used disk space in bytes for root (`/`) and a specified mount point (`/mnt/usb`).

* **Network Stats**: Real-time upload and download speeds in bytes/sec, as well as total bytes sent and received.

## Installation

1. **Compile the code**: The project requires the `pthread` library for threading.

   ```
   gcc -o system_monitor system_monitor.c -pthread


   ```

## Usage

1. **Run the executable**:

   You don't need `sudo` to run the compiled program. Just execute it directly.

   ```
   ./system_monitor


   ```

   The server will start and listen on port **3040**.

2. **Access the API**: You can send an HTTP GET request to the device's IP address on port 3040. For example, using `curl` on the same machine:

   ```
   curl http://localhost:3040


   ```

## Running as a Systemd Service

To ensure the service starts automatically on boot and runs in the background, you can set it up as a `systemd` service. This also allows you to manage it easily with `systemctl` commands without needing a running terminal session.

1. Create a service file at `/etc/systemd/system/system_monitor.service` with the following content:

   ```
   [Unit]
   Description=C System Monitor API

   [Service]
   ExecStart=/home/wan/system_monitor
   User=wan
   Restart=always

   [Install]
   WantedBy=multi-user.target


   ```

   * `ExecStart`: Make sure the path `/home/wan/system_monitor` is correct for your user.

   * `User`: Set this to your user account (`wan`). This is crucial as it ensures the service runs with your user's permissions and does not require `sudo`.

2. Reload the `systemd` daemon to recognize the new service:

   ```
   sudo systemctl daemon-reload


   ```

3. Enable and start the service:

   ```
   sudo systemctl enable system_monitor.service
   sudo systemctl start system_monitor.service


   ```

4. Check the status of the service:

   ```
   sudo systemctl status system_monitor.service


   ```

## API Output

The API returns a JSON object with the following fields:

```
{
  "cpu_uptime_seconds": 123456,
  "cpu_usage_percent": 25.50,
  "ram_total_kb": 1048576,
  "ram_used_kb": 524288,
  "swap_total_kb": 2097152,
  "swap_used_kb": 1048576,
  "cpu_temp_millicelsius": 45000,
  "net_upload_bytes_sec": 1500.25,
  "net_download_bytes_sec": 3400.75,
  "net_total_bytes_sent": 12345678,
  "net_total_bytes_recv": 87654321,
  "main_disk_total_bytes": 32212254720,
  "main_disk_used_bytes": 10737418240,
  "main_disk_usage_percent": 33.33,
  "usb_disk_total_bytes": 64424509440,
  "usb_disk_used_bytes": 5368709120,
  "usb_disk_usage_percent": 8.33
}
