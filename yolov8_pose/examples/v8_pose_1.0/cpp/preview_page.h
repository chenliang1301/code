#ifndef POSE_PREVIEW_PAGE_H
#define POSE_PREVIEW_PAGE_H
static const char kIndexHtml[] = R"HTML(
<!doctype html><html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>姿态与挥拍计数</title><style>
*{box-sizing:border-box}html,body{margin:0;background:#111;color:white;font:14px sans-serif;height:100%}
body{display:flex;flex-direction:column;overflow:auto}#tools{display:flex;flex-wrap:wrap;gap:8px;padding:8px;background:#222;align-items:center}
button,select{min-height:34px;padding:0 10px}details,#diagnostics{padding:8px 12px;background:#182028}
summary{cursor:pointer}#next{color:#ffe28a}#result{color:#9de0ff;white-space:pre-wrap}
#stage{position:relative;flex:1;min-height:240px;display:flex;align-items:center;justify-content:center}
#stream,#overlay{position:absolute;max-width:100%;max-height:100%;width:auto;height:auto;object-fit:contain}
#overlay{touch-action:none}#summary{overflow-wrap:anywhere}
</style></head><body>
<div id="tools"><button id="primary">开始引导</button><button data-command="pause">暂停</button><span id="state"></span></div>
<details><summary>更多设置 / ROI / 校准文件 / 清零</summary>
<label>ROI边数 <select id="sides"><option>3</option><option selected>4</option><option>5</option><option>6</option></select></label>
<button id="draw">绘制ROI</button><button id="undo">撤销点</button><button id="save">应用ROI</button><button id="clear">清除ROI</button><button id="fullRoi">全画面ROI</button>
<button data-command="load">加载校准</button><button data-command="save">保存校准</button><button data-command="calibrate_fh">重采正手</button><button data-command="calibrate_bh">重采反手</button><button data-command="reset">清零计数</button><button id="query">刷新状态</button></details>
<select id="tennis" hidden><option value="start">start</option><option value="confirm">confirm</option><option value="pause">pause</option><option value="calibrate_fh">fh</option><option value="calibrate_bh">bh</option><option value="save">save</option><option value="load">load</option><option value="reset">reset</option></select><button id="tennisSend" hidden>send</button>
<details><summary>使用步骤和注意事项（点击展开）</summary>
<ol><li>先调整机位：双肩、双髋、持拍侧肘腕及完整挥拍范围入镜。不要只拍脸部。</li>
<li>绘制ROI，在视频中依次点选顶点，再点击应用ROI；只包含一名训练者。修改ROI会清空校准。</li>
<li>采集正手：先站稳半秒，完整挥拍后回到原准备位置并停顿，重复到指定样本数。</li>
<li>采集反手：同样完成指定次数。两类样本通过可分性检查后自动计数。</li>
<li>保存校准。下次先应用ROI，再加载校准，确认人物、机位和模型未变后开始。</li></ol>
<p>FH=正手，BH=反手，UNKNOWN=完整挥拍但类别不确定；统计挥拍，不判断是否击中球。
采集命令会清空该类旧样本；暂停保留次数。动作结束后必须回到最初准备位置，才能计下一次。</p></details>
<section id="diagnostics"><div id="summary">正在连接状态接口…</div><div id="next"></div><div id="step"></div><div id="debug"></div><div id="result"></div></section>
<div id="stage"><img id="stream" src="/stream.mjpg"><canvas id="overlay"></canvas></div>
<script>
const img=document.querySelector('#stream'),canvas=document.querySelector('#overlay'),ctx=canvas.getContext('2d');
const stage=document.querySelector('#stage'),state=document.querySelector('#state'),result=document.querySelector('#result');
const send=document.querySelector('#tennisSend');let pts=[],saved=[],drawing=false,waiting=null,submitted=0,current=null;
function draw(){ctx.clearRect(0,0,canvas.width,canvas.height);const p=drawing?pts:saved;if(!p.length)return;
ctx.strokeStyle='#00ffff';ctx.fillStyle='#00ffff';ctx.lineWidth=2;ctx.beginPath();
p.forEach((v,i)=>{const x=v[0]*canvas.width,y=v[1]*canvas.height;i?ctx.lineTo(x,y):ctx.moveTo(x,y)});
if((drawing&&p.length===+document.querySelector('#sides').value)||(!drawing&&p.length>=3))ctx.closePath();ctx.stroke();
p.forEach(v=>{ctx.beginPath();ctx.arc(v[0]*canvas.width,v[1]*canvas.height,4,0,Math.PI*2);ctx.fill()})}
function fit(){const r=img.getBoundingClientRect(),s=stage.getBoundingClientRect();canvas.style.left=(r.left-s.left)+'px';
canvas.style.top=(r.top-s.top)+'px';canvas.style.width=r.width+'px';canvas.style.height=r.height+'px';canvas.width=Math.round(r.width);canvas.height=Math.round(r.height);draw()}
img.onload=fit;window.onresize=fit;new ResizeObserver(fit).observe(stage);
document.querySelector('#draw').onclick=()=>{drawing=true;pts=[];state.textContent='依次点选 '+document.querySelector('#sides').value+' 个顶点';draw()};
canvas.onclick=e=>{if(!drawing||pts.length>=+document.querySelector('#sides').value)return;const r=canvas.getBoundingClientRect();
if(!r.width||!r.height)return;pts.push([(e.clientX-r.left)/r.width,(e.clientY-r.top)/r.height]);draw();
if(pts.length===+document.querySelector('#sides').value)state.textContent='顶点已齐，请点击应用ROI'};
document.querySelector('#undo').onclick=()=>{if(drawing){pts.pop();draw()}};
document.querySelector('#save').onclick=async()=>{if(!drawing||pts.length!==+document.querySelector('#sides').value){state.textContent='请先绘制指定数量的顶点';return}
try{const r=await fetch('/roi',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({points:pts})});
if(!r.ok)throw Error('HTTP '+r.status);saved=pts.slice();drawing=false;state.textContent='ROI已应用，原校准会清空';draw();await query()}catch(e){state.textContent='ROI应用失败：'+e.message}};
document.querySelector('#clear').onclick=async()=>{try{const r=await fetch('/roi',{method:'DELETE'});if(!r.ok)throw Error('HTTP '+r.status);
saved=[];pts=[];drawing=false;state.textContent='ROI已清除，分析暂停';draw()}catch(e){state.textContent='清除失败：'+e.message}};
fetch('/roi').then(r=>r.json()).then(d=>{saved=d.points||[];fit()}).catch(e=>{state.textContent='读取ROI失败：'+e.message});
async function query(){try{const r=await fetch('/tennis/status',{cache:'no-store'});if(!r.ok)throw Error('HTTP '+r.status);const d=await r.json(),s=d.state;current=s;const primary=document.querySelector('#primary');primary.disabled=waiting!==null||!s.enabled||s.collecting>0;primary.textContent=!s.roi_applied?'① 单人全画面':s.collecting===1?'采集正手中，请挥拍':s.collecting===2?'采集反手中，请挥拍':s.calibrated?'开始 / 恢复计数':s.fh_samples<s.samples_required?'② 采集正手':s.bh_samples<s.samples_required?'③ 采集反手':'校准未通过：重新采集';
document.querySelector('#summary').textContent=s.enabled?`状态 ${s.status} | 动作 ${s.motion} | ROI人数 ${s.people} | 姿态${s.pose_ok?'可用':'不合格'} | 正手样本 ${s.fh_samples}/${s.samples_required} | 反手样本 ${s.bh_samples}/${s.samples_required} | FH ${s.fh} BH ${s.bh} UNKNOWN ${s.unknown} 拒绝 ${s.rejected} | 分析 ${Number(s.analysis_hz).toFixed(1)}Hz | 状态年龄 ${d.age_ms}ms`:'网球模式未启用';
document.querySelector('#next').textContent=(d.age_ms>3000&&s.enabled?'状态超过3秒未更新：检查摄像头或推理是否卡住，不要重复发命令。 ':'')+(s.advice||'')+(s.reason?' [诊断: '+s.reason+']':'');
document.querySelector('#step').textContent=s.step||'';
document.querySelector('#debug').textContent=s.debug?`调试：腕速 ${Number(s.wrist_speed).toFixed(2)}（启动 ${s.start_speed} / 停止 ${s.stop_speed}）| 静止 ${Number(s.ready_seconds).toFixed(2)}秒 | 动作 ${Number(s.action_seconds).toFixed(2)}秒 | 路径 ${Number(s.path).toFixed(2)} / 最小 ${s.min_path} | 回准备位距离 ${Number(s.return_distance).toFixed(2)} | ${s.pose_issue||''}`:'';
if(waiting!==null){const c=d.commands.find(c=>c.id===waiting);if(c){result.textContent='命令 #'+waiting+(c.result.ok?' 执行成功：':' 执行失败：')+c.result.message;waiting=null;send.disabled=false}
else if(Date.now()-submitted>15000){result.textContent='命令 #'+waiting+' 超时未确认，请查询状态并查看终端。执行结果未知，勿直接重发采样命令。';waiting=null;send.disabled=false}}
}catch(e){document.querySelector('#next').textContent='状态查询失败：'+e.message+'。检查进程、端口以及是否部署了新版本。'}}
send.onclick=async()=>{const cmd=document.querySelector('#tennis').value;
if((cmd==='reset'||cmd==='calibrate_fh'||cmd==='calibrate_bh')&&!confirm(cmd==='reset'?'确定清零计数？':'重新采集会清空该类旧样本，确定继续？'))return;
send.disabled=true;try{const r=await fetch('/tennis/'+cmd,{method:'POST'}),d=await r.json();if(!r.ok){result.textContent='命令拒绝：'+d.message;send.disabled=false;return}
waiting=d.id;submitted=Date.now();result.textContent='命令 #'+d.id+' 已排队，等待分析线程执行，尚未成功';await query()}
catch(e){result.textContent='发送失败，结果未知，请查询状态：'+e.message;send.disabled=false}};
document.querySelector('#primary').onclick=()=>{if(!current)return;if(!current.roi_applied){document.querySelector('#fullRoi').click();return}document.querySelector('#tennis').value=current.calibrated?'confirm':current.fh_samples<current.samples_required?'calibrate_fh':current.bh_samples<current.samples_required?'calibrate_bh':'calibrate_fh';send.click()};
document.querySelector('#query').onclick=query;setInterval(query,1000);query();
document.querySelectorAll('[data-command]').forEach(button=>button.onclick=()=>{if(send.disabled)return;document.querySelector('#tennis').value=button.dataset.command;send.click()});
document.querySelector('#fullRoi').onclick=async()=>{if(!confirm('全画面只允许一人。应用会清空已有校准，继续？'))return;try{const points=[[0,0],[1,0],[1,1],[0,1]];const r=await fetch('/roi',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({points})});if(!r.ok)throw Error('HTTP '+r.status);saved=points;drawing=false;draw();state.textContent='全画面ROI已应用，可直接点击①采集正手';await query()}catch(e){state.textContent='应用失败：'+e.message}};
</script></body></html>
)HTML";
#endif
