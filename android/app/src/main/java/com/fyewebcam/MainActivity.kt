package com.fyewebcam

import android.Manifest
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.os.IBinder
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.camera.view.PreviewView
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.content.ContextCompat
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import kotlinx.coroutines.delay

private val Ground = Color(0xFF0F1115)
private val Surface = Color(0xFF171A20)
private val Line = Color(0xFF2A2F38)
private val Muted = Color(0xFFA3AAB6)
private val Body = Color(0xFFC4C9D2)
private val Accent = Color(0xFF2DD4BF)

class MainActivity : ComponentActivity() {

    private var service by mutableStateOf<StreamService?>(null)

    private val connection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, binder: IBinder?) {
            service = (binder as StreamService.LocalBinder).service
        }

        override fun onServiceDisconnected(name: ComponentName?) {
            service = null
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            MaterialTheme(colorScheme = darkColorScheme(background = Ground, surface = Surface, primary = Accent)) {
                App(service = service, onStart = ::startStreaming, onStop = ::stopStreaming)
            }
        }
    }

    override fun onStart() {
        super.onStart()
        if (StreamState.state.value.running) bind()
    }

    override fun onStop() {
        service?.setPreviewSurfaceProvider(null)
        unbindQuietly()
        super.onStop()
    }

    private fun bind() {
        bindService(Intent(this, StreamService::class.java), connection, Context.BIND_AUTO_CREATE)
    }

    private fun unbindQuietly() {
        if (service != null) {
            unbindService(connection)
            service = null
        }
    }

    private fun startStreaming() {
        StreamService.start(this)
        bind()
    }

    private fun stopStreaming() {
        service?.setPreviewSurfaceProvider(null)
        unbindQuietly()
        StreamService.stop(this)
    }
}

@Composable
private fun App(service: StreamService?, onStart: () -> Unit, onStop: () -> Unit) {
    val context = androidx.compose.ui.platform.LocalContext.current
    val state by StreamState.state.collectAsStateWithLifecycle()
    var hasCamera by remember {
        mutableStateOf(ContextCompat.checkSelfPermission(context, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED)
    }
    val permissions = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { result ->
        hasCamera = result[Manifest.permission.CAMERA] == true
        if (hasCamera) onStart()
    }
    var addresses by remember { mutableStateOf(NetUtils.localIpv4Addresses()) }
    LaunchedEffect(Unit) {
        while (true) {
            addresses = NetUtils.localIpv4Addresses()
            delay(3000)
        }
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(Ground)
            .safeDrawingPadding()
            .padding(horizontal = 20.dp, vertical = 16.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        Text("fyewebcam", color = Color.White, fontSize = 20.sp, fontWeight = FontWeight.Bold)

        PreviewBox(service = service, state = state)

        Card {
            Label("Connect from your PC")
            if (addresses.isEmpty()) {
                Text("Connect the phone to Wi-Fi, or use USB.", color = Body, fontSize = 14.sp)
            }
            addresses.forEach { ip -> InfoRow("Wi-Fi IP", ip) }
            InfoRow("Port", Protocol.DEFAULT_PORT.toString())
            Text(
                "Or plug in a USB cable and pick USB in the Windows app.",
                color = Muted, fontSize = 13.sp,
            )
        }

        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Label("Resolution")
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .clip(RoundedCornerShape(12.dp))
                    .background(Surface)
                    .border(1.dp, Line, RoundedCornerShape(12.dp))
                    .padding(4.dp),
                horizontalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                Preset.entries.forEach { preset ->
                    val selected = preset == state.preset
                    Button(
                        onClick = {
                            if (service != null) service.applySettings(preset = preset)
                            else StreamState.update { it.copy(preset = preset) }
                        },
                        modifier = Modifier.weight(1f).height(44.dp),
                        shape = RoundedCornerShape(9.dp),
                        colors = ButtonDefaults.buttonColors(
                            containerColor = if (selected) Line else Color.Transparent,
                            contentColor = if (selected) Color.White else Body,
                        ),
                    ) { Text(preset.label, fontWeight = if (selected) FontWeight.SemiBold else FontWeight.Medium) }
                }
            }
        }

        Spacer(Modifier.weight(1f))

        state.error?.let { Text(it, color = Color(0xFFF97316), fontSize = 13.sp) }

        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            OutlinedButton(
                onClick = {
                    val next = 1 - state.camera
                    if (service != null) service.applySettings(camera = next)
                    else StreamState.update { it.copy(camera = next) }
                },
                modifier = Modifier.height(56.dp),
                shape = RoundedCornerShape(16.dp),
            ) { Text(if (state.camera == Protocol.CAMERA_FRONT) "Front" else "Back", color = Color.White) }
            Button(
                onClick = {
                    if (state.running) {
                        onStop()
                    } else if (hasCamera) {
                        onStart()
                    } else {
                        val wanted = mutableListOf(Manifest.permission.CAMERA)
                        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) wanted += Manifest.permission.POST_NOTIFICATIONS
                        permissions.launch(wanted.toTypedArray())
                    }
                },
                modifier = Modifier.weight(1f).height(56.dp),
                shape = RoundedCornerShape(16.dp),
                colors = ButtonDefaults.buttonColors(containerColor = Color(0xFFF2F3F5), contentColor = Ground),
            ) {
                Text(if (state.running) "Stop streaming" else "Start streaming", fontSize = 16.sp, fontWeight = FontWeight.SemiBold)
            }
        }
        Text(
            "Keeps streaming with the screen off",
            color = Muted, fontSize = 12.sp,
            modifier = Modifier.align(Alignment.CenterHorizontally),
        )
    }
}

@Composable
private fun PreviewBox(service: StreamService?, state: StreamState.Value) {
    Box(
        modifier = Modifier
            .fillMaxWidth()
            .height(240.dp)
            .clip(RoundedCornerShape(20.dp))
            .background(Color(0xFF1B1F27))
            .border(1.dp, Line, RoundedCornerShape(20.dp)),
        contentAlignment = Alignment.Center,
    ) {
        if (service != null && state.running) {
            AndroidView(
                factory = { ctx -> PreviewView(ctx).apply { implementationMode = PreviewView.ImplementationMode.COMPATIBLE } },
                modifier = Modifier.fillMaxSize(),
            ) { view -> service.setPreviewSurfaceProvider(view.surfaceProvider) }
            DisposableEffect(service) { onDispose { service.setPreviewSurfaceProvider(null) } }
        } else {
            Text("Camera is off", color = Color(0xFF8B93A1), fontSize = 13.sp)
        }
        if (state.running) {
            Row(
                modifier = Modifier
                    .align(Alignment.TopStart)
                    .padding(12.dp)
                    .clip(RoundedCornerShape(999.dp))
                    .background(Ground)
                    .padding(horizontal = 10.dp, vertical = 6.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                Box(Modifier.size(8.dp).clip(CircleShape).background(if (state.viewers > 0) Accent else Muted))
                val label = if (state.viewers > 0) "LIVE · ${state.viewers} viewer" else "Waiting for PC"
                Text(label, color = Color.White, fontSize = 12.sp, fontWeight = FontWeight.SemiBold)
            }
            if (state.height > 0) {
                Text(
                    "${state.height}p · ${state.fps} fps",
                    color = Body, fontSize = 12.sp, fontFamily = FontFamily.Monospace,
                    modifier = Modifier
                        .align(Alignment.BottomEnd)
                        .padding(12.dp)
                        .clip(RoundedCornerShape(8.dp))
                        .background(Ground)
                        .padding(horizontal = 10.dp, vertical = 6.dp),
                )
            }
        }
    }
}

@Composable
private fun Card(content: @Composable () -> Unit) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(16.dp))
            .background(Surface)
            .border(1.dp, Line, RoundedCornerShape(16.dp))
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) { content() }
}

@Composable
private fun Label(text: String) {
    Text(text.uppercase(), color = Muted, fontSize = 12.sp, fontWeight = FontWeight.SemiBold, letterSpacing = 0.7.sp)
}

@Composable
private fun InfoRow(label: String, value: String) {
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween, verticalAlignment = Alignment.CenterVertically) {
        Text(label, color = Body, fontSize = 14.sp)
        Text(value, color = Color.White, fontSize = 18.sp, fontWeight = FontWeight.SemiBold, fontFamily = FontFamily.Monospace)
    }
}
