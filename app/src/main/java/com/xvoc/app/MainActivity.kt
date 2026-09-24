package com.xvoc.app

import android.media.MediaPlayer
import android.net.Uri
import android.os.Bundle
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.snapshots.SnapshotStateList
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            MaterialTheme(colorScheme = darkColorScheme()) {
                Surface(modifier = Modifier.fillMaxSize()) {
                    XvocScreen()
                }
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun XvocScreen() {
    val context = androidx.compose.ui.platform.LocalContext.current
    val scope = rememberCoroutineScope()

    val d = remember { mutableStateListOf(*defaultDoubles().toTypedArray()) }
    val ip = remember { mutableStateListOf(*defaultInts().toTypedArray()) }

    var inputWav by remember { mutableStateOf<File?>(null) }
    var inputInfo by remember { mutableStateOf("No audio imported yet") }
    var outputWav by remember { mutableStateOf<File?>(null) }
    var isProcessing by remember { mutableStateOf(false) }
    var isPlaying by remember { mutableStateOf(false) }
    var player by remember { mutableStateOf<MediaPlayer?>(null) }

    fun toDoubleArray() = DoubleArray(d.size) { d[it] }
    fun toIntArray() = IntArray(ip.size) { ip[it] }

    fun stopPlayback() {
        player?.let { it.stop(); it.release() }
        player = null
        isPlaying = false
    }

    fun process(autoPlay: Boolean) {
        val src = inputWav ?: run {
            Toast.makeText(context, "Import an audio file first", Toast.LENGTH_SHORT).show()
            return
        }
        stopPlayback()
        isProcessing = true
        scope.launch {
            val outFile = File(context.cacheDir, "xvoc_out.wav")
            val rc = withContext(Dispatchers.Default) {
                NativeVocoder.process(src.absolutePath, outFile.absolutePath, toDoubleArray(), toIntArray())
            }
            isProcessing = false
            if (rc == 0) {
                outputWav = outFile
                if (autoPlay) {
                    player = MediaPlayer().apply {
                        setDataSource(outFile.absolutePath)
                        setOnCompletionListener { isPlaying = false }
                        prepare()
                        start()
                    }
                    isPlaying = true
                }
            } else {
                Toast.makeText(context, "Processing failed (code $rc)", Toast.LENGTH_SHORT).show()
            }
        }
    }

    val importLauncher = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
        if (uri == null) return@rememberLauncherForActivityResult
        scope.launch {
            val dest = File(context.cacheDir, "xvoc_in.wav")
            val ok = withContext(Dispatchers.IO) { AudioIO.importToWav(context, uri, dest) }
            if (ok) {
                inputWav = dest
                val probe = withContext(Dispatchers.Default) { NativeVocoder.probe(dest.absolutePath) }
                inputInfo = if (probe != null) {
                    val secs = probe[0].toDouble() / probe[1].toDouble()
                    "%.1fs @ %dHz".format(secs, probe[1])
                } else "Imported (could not read back)"
                outputWav = null
            } else {
                Toast.makeText(context, "Couldn't decode that file", Toast.LENGTH_SHORT).show()
            }
        }
    }

    val exportLauncher = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("audio/wav")) { uri: Uri? ->
        if (uri == null) return@rememberLauncherForActivityResult
        val out = outputWav ?: return@rememberLauncherForActivityResult
        scope.launch {
            val ok = withContext(Dispatchers.IO) { AudioIO.exportTo(context, out, uri) }
            Toast.makeText(context, if (ok) "Exported" else "Export failed", Toast.LENGTH_SHORT).show()
        }
    }

    DisposableEffect(Unit) {
        onDispose { stopPlayback() }
    }

    Column(modifier = Modifier.fillMaxSize()) {
        TopAppBar(title = { Text("xvoc") })

        Row(
            modifier = Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 6.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            OutlinedButton(onClick = { importLauncher.launch(arrayOf("audio/*")) }, modifier = Modifier.weight(1f)) {
                Icon(Icons.Filled.FileOpen, contentDescription = null, modifier = Modifier.size(18.dp))
                Spacer(Modifier.width(4.dp))
                Text("Import")
            }
            Button(
                onClick = { process(autoPlay = true) },
                enabled = inputWav != null && !isProcessing,
                modifier = Modifier.weight(1f)
            ) {
                if (isProcessing) {
                    CircularProgressIndicator(modifier = Modifier.size(16.dp), strokeWidth = 2.dp)
                } else {
                    Icon(if (isPlaying) Icons.Filled.Stop else Icons.Filled.PlayArrow, contentDescription = null, modifier = Modifier.size(18.dp))
                }
                Spacer(Modifier.width(4.dp))
                Text(if (isPlaying) "Stop" else "Render & Play")
            }
            OutlinedButton(
                onClick = {
                    val out = DoubleArray(Idx.D_COUNT)
                    val outI = IntArray(Idx.I_COUNT)
                    NativeVocoder.randomize(System.nanoTime(), out, outI)
                    for (k in out.indices) d[k] = out[k]
                    for (k in outI.indices) ip[k] = outI[k]
                    if (inputWav != null) process(autoPlay = true)
                },
                modifier = Modifier.weight(1f)
            ) {
                Icon(Icons.Filled.Shuffle, contentDescription = null, modifier = Modifier.size(18.dp))
                Spacer(Modifier.width(4.dp))
                Text("Randomize")
            }
        }

        Row(
            modifier = Modifier.fillMaxWidth().padding(horizontal = 12.dp),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text(inputInfo, style = MaterialTheme.typography.bodySmall)
            TextButton(
                onClick = { exportLauncher.launch("xvoc_output.wav") },
                enabled = outputWav != null
            ) {
                Icon(Icons.Filled.Download, contentDescription = null, modifier = Modifier.size(16.dp))
                Spacer(Modifier.width(4.dp))
                Text("Export")
            }
        }
        HorizontalDivider(modifier = Modifier.padding(top = 4.dp))

        LazyColumn(modifier = Modifier.fillMaxWidth().weight(1f)) {
            items(SECTIONS) { section ->
                Text(
                    section.title,
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.Bold,
                    modifier = Modifier.padding(start = 16.dp, top = 16.dp, bottom = 4.dp)
                )
                section.params.forEach { p ->
                    ParamRow(p, d, ip)
                }
            }
            item { Spacer(Modifier.height(32.dp)) }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ParamRow(p: Param, d: SnapshotStateList<Double>, ip: SnapshotStateList<Int>) {
    Column(modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 2.dp)) {
        when (p.kind) {
            Kind.SLIDER_D -> {
                val v = d[p.index]
                Text("${p.label}: ${"%.2f".format(v)}${if (p.unit.isNotEmpty()) " ${p.unit}" else ""}", style = MaterialTheme.typography.bodyMedium)
                Slider(
                    value = v.toFloat(),
                    onValueChange = { d[p.index] = it.toDouble() },
                    valueRange = p.min..p.max
                )
            }
            Kind.SLIDER_I -> {
                val v = ip[p.index]
                Text("${p.label}: $v${if (p.unit.isNotEmpty()) " ${p.unit}" else ""}", style = MaterialTheme.typography.bodyMedium)
                Slider(
                    value = v.toFloat(),
                    onValueChange = { ip[p.index] = it.toInt() },
                    valueRange = p.min..p.max,
                    steps = (p.max - p.min).toInt() - 1
                )
            }
            Kind.BOOL -> {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Text(p.label, style = MaterialTheme.typography.bodyMedium)
                    Switch(
                        checked = ip[p.index] != 0,
                        onCheckedChange = { ip[p.index] = if (it) 1 else 0 }
                    )
                }
            }
            Kind.ENUM -> {
                var expanded by remember { mutableStateOf(false) }
                val isBits = p.key == "bits"
                val selectedIndex = if (isBits) (if (ip[p.index] == 24) 1 else 0) else ip[p.index]
                val selectedLabel = p.options.getOrElse(selectedIndex) { "?" }
                Text(p.label, style = MaterialTheme.typography.bodyMedium)
                ExposedDropdownMenuBox(expanded = expanded, onExpandedChange = { expanded = it }) {
                    OutlinedTextField(
                        value = selectedLabel,
                        onValueChange = {},
                        readOnly = true,
                        modifier = Modifier.menuAnchor().fillMaxWidth(),
                        trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = expanded) }
                    )
                    ExposedDropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
                        p.options.forEachIndexed { idx, opt ->
                            DropdownMenuItem(text = { Text(opt) }, onClick = {
                                ip[p.index] = if (isBits) (if (idx == 1) 24 else 16) else idx
                                expanded = false
                            })
                        }
                    }
                }
            }
        }
    }
}
