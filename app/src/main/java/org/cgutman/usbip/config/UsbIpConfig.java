package org.cgutman.usbip.config;

import org.cgutman.usbip.server.UsbIpServer;
import org.cgutman.usbip.service.UsbIpService;
import org.cgutman.usbipserverforandroid.R;

import android.Manifest;
import android.app.ActivityManager;
import android.app.ActivityManager.RunningServiceInfo;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.view.View;
import android.view.View.OnClickListener;
import android.widget.Button;
import android.widget.TextView;

import androidx.activity.ComponentActivity;
import androidx.activity.result.ActivityResultLauncher;
import androidx.activity.result.contract.ActivityResultContracts;
import androidx.core.content.ContextCompat;

import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.NetworkInterface;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Enumeration;
import java.util.List;

public class UsbIpConfig extends ComponentActivity {
	private Button serviceButton;
	private TextView serviceStatus;
	private TextView serviceReadyText;
	
	private boolean running;

	private ActivityResultLauncher<String> requestPermissionLauncher =
			registerForActivityResult(new ActivityResultContracts.RequestPermission(), isGranted -> {
				// We don't actually care if the permission is granted or not. We will launch the service anyway.
				startService(new Intent(UsbIpConfig.this, UsbIpService.class));
			});
	
	// Returns labelled IPv4 addresses reachable on this device, e.g.
	//   "wlan0: 192.168.1.34"  (Wi-Fi STA)
	//   "ap0:   192.168.43.1"  (hotspot AP)
	// We list them all because Android's hotspot interface usually has a
	// different name and address than the STA Wi-Fi, and clients connecting
	// over the hotspot must use the AP IP, not the STA one.
	private static List<String> listReachableIPv4() {
		List<String> out = new ArrayList<>();
		try {
			Enumeration<NetworkInterface> ifs = NetworkInterface.getNetworkInterfaces();
			if (ifs == null) return out;
			for (NetworkInterface ni : Collections.list(ifs)) {
				if (!ni.isUp() || ni.isLoopback()) continue;
				for (InetAddress ia : Collections.list(ni.getInetAddresses())) {
					if (ia instanceof Inet4Address && !ia.isLoopbackAddress() && !ia.isLinkLocalAddress()) {
						out.add(ni.getName() + ": " + ia.getHostAddress());
					}
				}
			}
		} catch (Exception e) {
			// Best effort; if NetworkInterface enumeration fails we just show fewer hints.
		}
		return out;
	}

	private void updateStatus() {
		if (running) {
			serviceButton.setText("Stop Service");
			serviceStatus.setText("USB/IP Service Running");

			StringBuilder sb = new StringBuilder();
			sb.append(getString(R.string.ready_text));
			sb.append("\n\nListening on TCP port ").append(UsbIpServer.PORT).append(" on:\n");
			List<String> ips = listReachableIPv4();
			if (ips.isEmpty()) {
				sb.append("(no active IPv4 interface detected)");
			} else {
				for (String ip : ips) {
					sb.append("  ").append(ip).append('\n');
				}
			}
			serviceReadyText.setText(sb.toString());
		}
		else {
			serviceButton.setText("Start Service");
			serviceStatus.setText("USB/IP Service Stopped");
			serviceReadyText.setText("");
		}
	}

	@Override
	protected void onResume() {
		super.onResume();
		// Re-check service state and refresh the IP list whenever the user
		// returns to the activity (e.g. after toggling Wi-Fi / hotspot).
		running = isMyServiceRunning(UsbIpService.class);
		updateStatus();
	}
	
	// Elegant Stack Overflow solution to querying running services
	private boolean isMyServiceRunning(Class<?> serviceClass) {
	    ActivityManager manager = (ActivityManager) getSystemService(Context.ACTIVITY_SERVICE);
	    for (RunningServiceInfo service : manager.getRunningServices(Integer.MAX_VALUE)) {
	        if (serviceClass.getName().equals(service.service.getClassName())) {
	            return true;
	        }
	    }
	    return false;
	}
	
	@Override
	protected void onCreate(Bundle savedInstanceState) {
		super.onCreate(savedInstanceState);
		setContentView(R.layout.activity_usbip_config);

		serviceButton = findViewById(R.id.serviceButton);
		serviceStatus = findViewById(R.id.serviceStatus);
		serviceReadyText = findViewById(R.id.serviceReadyText);
		
		running = isMyServiceRunning(UsbIpService.class);
		
		updateStatus();
		
		serviceButton.setOnClickListener(new OnClickListener() {
			@Override
			public void onClick(View v) {
				if (running) {
					stopService(new Intent(UsbIpConfig.this, UsbIpService.class));
				}
				else {
					if (ContextCompat.checkSelfPermission(UsbIpConfig.this, Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED) {
						startService(new Intent(UsbIpConfig.this, UsbIpService.class));
					} else {
						requestPermissionLauncher.launch(Manifest.permission.POST_NOTIFICATIONS);
					}
				}
				
				running = !running;
				updateStatus();
			}
		});
	}
}
