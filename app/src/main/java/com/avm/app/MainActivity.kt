package com.avm.app

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * Stage 1 UI: 엔진 셀프테스트 콘솔.
 *
 * 이 화면은 임시 개발 콘솔이며, Stage 9에서 VM 목록/설정/실행 화면으로
 * 대체된다. 단, "네이티브 엔진이 게스트 코드를 실제로 실행한 결과를
 * 보여준다"는 원칙은 지금부터 지킨다 — 표시되는 로그는 전부 게스트
 * RAM에서 실행된 ARM64 명령어의 실측 결과다.
 */
class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            MaterialTheme {
                SelfTestScreen()
            }
        }
    }
}

@Composable
private fun SelfTestScreen() {
    var consoleText by remember { mutableStateOf("AVM — 독자 ARM64 시스템 에뮬레이터\n${NativeBridge.engineVersion()}\n\n[셀프테스트 실행]을 누르면 내장 ARM64 게스트 바이너리를\n가상 CPU에서 실행하고 결과를 검증합니다.") }
    var running by remember { mutableStateOf(false) }
    val scope = rememberCoroutineScope()

    Scaffold { innerPadding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(innerPadding)
                .padding(12.dp),
        ) {
            Row(modifier = Modifier.fillMaxWidth()) {
                Button(
                    enabled = !running,
                    onClick = {
                        running = true
                        scope.launch {
                            // vCPU 실행은 UI 스레드 밖에서 — Stage 9의 vCPU
                            // 전용 스레드 구조의 축소판.
                            val log = withContext(Dispatchers.Default) {
                                NativeBridge.runCpuSelfTest()
                            }
                            consoleText = log
                            running = false
                        }
                    },
                ) {
                    Text(if (running) "실행 중…" else "셀프테스트 실행")
                }
                Spacer(modifier = Modifier.width(12.dp))
            }

            Text(
                text = consoleText,
                fontFamily = FontFamily.Monospace,
                fontSize = 12.sp,
                color = Color(0xFFB9F6CA),
                modifier = Modifier
                    .fillMaxSize()
                    .padding(top = 12.dp)
                    .background(Color(0xFF101314))
                    .padding(8.dp)
                    .verticalScroll(rememberScrollState()),
            )
        }
    }
}
