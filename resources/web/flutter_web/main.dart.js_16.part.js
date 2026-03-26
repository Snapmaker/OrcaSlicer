((a,b,c)=>{a[b]=a[b]||{}
a[b][c]=a[b][c]||[]
a[b][c].push({p:"main.dart.js_16",e:"beginPart"})})(self,"$__dart_deferred_initializers__","eventLog")
$__dart_deferred_initializers__.current=function(a,b,c,$){var J,A,B,G,H,C={
c2q(){return new C.wn(null)},
wn:function wn(d){this.a=d},
WZ:function WZ(){var _=this
_.d=!1
_.e=""
_.f=!0
_.r=!1
_.w=0
_.y=_.x=null
_.z=!1
_.c=_.a=_.Q=null},
bhM:function bhM(d){this.a=d},
bhN:function bhN(d){this.a=d},
bhO:function bhO(d){this.a=d},
bhP:function bhP(d,e){this.a=d
this.b=e},
bhG:function bhG(d){this.a=d},
bhH:function bhH(d){this.a=d},
bhI:function bhI(d,e,f){this.a=d
this.b=e
this.c=f},
bhJ:function bhJ(d){this.a=d},
bhK:function bhK(d){this.a=d},
bhL:function bhL(d){this.a=d},
bhR:function bhR(d){this.a=d},
bhS:function bhS(d){this.a=d},
bhT:function bhT(d){this.a=d},
bhQ:function bhQ(d){this.a=d},
Xk:function Xk(d,e){this.c=d
this.a=e},
Xl:function Xl(d,e,f){var _=this
_.d=d
_.e=e
_.f=""
_.r=f
_.w=-1
_.c=_.a=null},
bj8:function bj8(d,e){this.a=d
this.b=e},
bj7:function bj7(d){this.a=d},
bj9:function bj9(d){this.a=d},
bja:function bja(d,e){this.a=d
this.b=e},
c_N(d){var x=new self.Blob(d)
return x}},D,F,E,I
J=c[1]
A=c[0]
B=c[2]
G=c[9]
H=c[24]
C=a.updateHolder(c[8],C)
D=c[25]
F=c[14]
E=c[17]
I=c[18]
C.wn.prototype={
Z(){return new C.WZ()}}
C.WZ.prototype={
ak(){this.aw()
this.zx()},
n(){this.au()},
zx(){var x=0,w=A.o(y.b),v=1,u,t=[],s=this,r,q,p,o,n,m,l,k
var $async$zx=A.k(function(d,e){if(d===1){u=e
x=v}while(true)switch(x){case 0:$.ay.$1("DownloadFile _init()")
s.M(new C.bhM(s))
v=3
$.ar().$1$0(y.a).n0("Loading...",y.b)
m=$.b6()
r=m.gct()
q=m.geg()
x=6
return A.e(q.iB(),$async$zx)
case 6:p=e
if(p==null){$.ay.$1("wcp.getConnectedMachine is null")
m=A.av("Cannot get connected machine")
throw A.h(m)}$.ay.$1("DeviceDetail getConnectedMachine result: "+p.P().j(0))
r.n_(p.P())
m=s.Q
if(m!=null)m.L(0)
m=$.hR
if(m==null)m=$.hR=A.qw()
s.Q=m.z7(p).bI(new C.bhN(s))
x=7
return A.e(q.q4(p),$async$zx)
case 7:o=e
if(!o){m=A.av("Connect failed")
throw A.h(m)}$.ay.$1("DeviceDetail connectDevice & subscribeMachineState")
s.M(new C.bhO(s))
t.push(5)
x=4
break
case 3:v=2
k=u
n=A.y(k)
$.ay.$1("DownloadFile _init() error: "+J.t(n))
s.M(new C.bhP(s,n))
t.push(5)
x=4
break
case 2:t=[1]
case 4:v=1
$.ar().$1$0(y.a).eD()
x=t.pop()
break
case 5:return A.m(null,w)
case 1:return A.l(u,w)}})
return A.n($async$zx,w)},
DR(d){return this.aGY(d)},
aGY(a0){var x=0,w=A.o(y.q),v=1,u,t=[],s=this,r,q,p,o,n,m,l,k,j,i,h,g,f,e,d
var $async$DR=A.k(function(a1,a2){if(a1===1){u=a2
x=v}while(true)switch(x){case 0:f=y.w
x=2
return A.e($.b6().gct().CC(A.B(["is_zip",a0],f,y.b)),$async$DR)
case 2:e=a2
x=e!=null?3:5
break
case 3:j=e.b
i=J.a7(j)
h=i.h(j,"data")
r=h==null?null:J.u(h,"file_name")
j=i.h(j,"data")
q=j==null?null:J.u(j,"file_path")
j="DownloadFile _downloadFileForWeb(isZip: "+a0
$.ay.$1(j+") filename: "+A.d(r))
$.ay.$1(j+") filePath: "+A.d(q))
v=7
s.M(new C.bhG(s))
p=A.dD("http://127.0.0.1:13619/localfile/"+s.b5o(q),0,null)
$.ay.$1("DownloadFile _downloadFileForWeb() uri: "+A.d(p))
x=10
return A.e(A.bGR(p),$async$DR)
case 10:o=a2
s.M(new C.bhH(s))
$.ay.$1("DownloadFile _downloadFileForWeb() response.statusCode: "+o.b)
j=$.ay
i=o
A.Ls(A.Lh(i.e)).er(0,i.w)
j.$1("DownloadFile _downloadFileForWeb() response.body.runtimeType: "+A.cw(f).j(0))
$.ay.$1("DownloadFile _downloadFileForWeb() response.body: *****")
if(o.b===200){n=o.w
j=$.ay
i=o
A.Ls(A.Lh(i.e)).er(0,i.w)
i=A.cw(f).j(0)
o.toString
j.$1("DownloadFile _downloadFileForWeb() http.body.runtimeType: "+i+", http.bodyBytes.runtimeType:"+B.y2.j(0))
s.M(new C.bhI(s,n,r))
m=C.c_N([o.w])
s.M(new C.bhJ(s))
i=(self.URL||self.webkitURL).createObjectURL(m)
i.toString
l=i
i=l
j=document.createElement("a")
j.toString
if(i!=null)j.href=i
j.setAttribute("download",A.d(r)+".gcode")
j.click();(self.URL||self.webkitURL).revokeObjectURL(l)
s.M(new C.bhK(s))
$.ay.$1("DownloadFile _downloadFileForWeb() \u4e0b\u8f7d\u6210\u529f")
j=s.c
if(j!=null){j=j.az(y.v)
j.toString
i=A.d(r)
h=o
A.Ls(A.Lh(h.e)).er(0,h.w)
j.f.mg(F.nd(null,null,null,E.i9,null,B.C,null,A.U("\u6587\u4ef6 "+i+".gcode \u4e0b\u8f7d\u6210\u529f\uff0c\u5185\u5bb9\u5df2\u52a0\u8f7d, boby.runtimeType: "+A.cw(f).j(0),null,null,null,null,null,null,null),null,E.du,null,null,null,null,null,null,null,null,null))}}else{f=A.av("\u4e0b\u8f7d\u5931\u8d25: HTTP "+o.b)
throw A.h(f)}t.push(9)
x=8
break
case 7:v=6
d=u
k=A.y(d)
$.ay.$1("DownloadFile _downloadFileForWeb() error: "+J.t(k))
f=A.av("\u4e0b\u8f7d\u6587\u4ef6\u65f6\u53d1\u751f\u9519\u8bef: "+A.d(k))
throw A.h(f)
t.push(9)
x=8
break
case 6:t=[1]
case 8:v=1
s.M(new C.bhL(s))
x=t.pop()
break
case 9:x=4
break
case 5:$.ay.$1("DownloadFile _downloadFileForWeb() activeFile is null")
throw A.h(A.av("\u65e0\u6cd5\u83b7\u53d6\u6d3b\u52a8\u6587\u4ef6\u4fe1\u606f"))
case 4:return A.m(null,w)
case 1:return A.l(u,w)}})
return A.n($async$DR,w)},
DQ(d){return this.aGX(d)},
aGX(d){var x=0,w=A.o(y.q),v=1,u,t=this,s,r,q,p
var $async$DQ=A.k(function(e,f){if(e===1){u=f
x=v}while(true)switch(x){case 0:v=3
x=6
return A.e(t.DR(d),$async$DQ)
case 6:v=1
x=5
break
case 3:v=2
p=u
s=A.y(p)
$.ay.$1("DownloadFile _downloadFile() error: "+J.t(s))
q=t.c
if(q!=null){q=q.az(y.v)
q.toString
q.f.mg(F.nd(null,null,null,B.md,null,B.C,null,A.U("\u4e0b\u8f7d\u5931\u8d25: "+J.t(s),null,null,null,null,null,null,null),null,E.du,null,null,null,null,null,null,null,null,null))}x=5
break
case 2:x=1
break
case 5:return A.m(null,w)
case 1:return A.l(u,w)}})
return A.n($async$DQ,w)},
b5o(d){var x,w,v,u
for(x=new A.T2(d),w="";x.t();){v=A.dL(x.d)
u=A.aV("[\\u4e00-\\u9fa5#]",!0,!1,!1)
w=u.b.test(v)?w+A.vs(E.Gj,v,B.ag,!1):w+v}return w.charCodeAt(0)==0?w:w},
D(d){var x,w,v,u,t,s,r,q=this,p=null,o=A.Eb(p,p,p,p,p,p,p,D.baK,p)
if(q.f)x=B.eW
else{x=y.u
if(q.d)x=A.cm(A.aX(A.a([D.abZ,B.aG,A.U("\u8fde\u63a5\u9519\u8bef",p,p,p,p,A.w(d).p2.f,p,p),B.aP,A.U(q.e,p,p,p,p,A.w(d).p2.z,B.bb,p),B.aG,A.fr(!1,B.Ym,p,p,B.a7,p,p,p,p,q.gaH0(),p,p)],x),B.p,B.ad,B.m,B.v),p,p)
else{w=G.aBI(new A.aY(B.d8,A.aX(A.a([A.U("\u6587\u4ef6\u4e0b\u8f7d",p,p,p,p,A.w(d).p2.f,p,p),B.aP,A.U("\u70b9\u51fb\u4e0b\u65b9\u6309\u94ae\u4e0b\u8f7d\u5f53\u524d\u6d3b\u52a8\u7684GCode\u6587\u4ef6",p,p,p,p,A.w(d).p2.z,p,p)],x),B.aa,B.q,B.m,B.v),p),p,p)
v=q.r
u=v?p:new C.bhR(q)
t=v?D.Xh:D.Ci
u=F.aIF(t,A.U(v?"\u4e0b\u8f7d\u4e2d...":"\u4e0b\u8f7dGCode\u6587\u4ef6",p,p,p,p,p,p,p),u,A.kc(p,p,p,p,p,p,p,p,p,p,p,p,p,D.Bl,p,p,p,p,p,p,p,p))
t=v?p:new C.bhS(q)
s=v?D.Xh:D.Ci
w=A.a([w,B.aG,u,B.aG,F.aIF(s,A.U(v?"\u4e0b\u8f7d\u4e2d...":"\u4e0b\u8f7dZIP\u6587\u4ef6",p,p,p,p,p,p,p),t,A.kc(p,p,p,p,p,p,p,p,p,p,p,p,p,D.Bl,p,p,p,p,p,p,p,p))],x)
if(q.r)B.b.G(w,A.a([B.aG,G.aBI(new A.aY(B.d8,A.aX(A.a([A.bc(A.a([A.U("\u4e0b\u8f7d\u8fdb\u5ea6",p,p,p,p,A.w(d).p2.w,p,p),A.U(""+B.d.ao(q.w*100)+"%",p,p,p,p,A.w(d).p2.w,p,p)],x),B.p,B.cJ,B.m,p),B.aP,F.bDC(B.d7,B.b4,p,p,q.w,new A.kF(B.rZ,y.e))],x),B.aa,B.q,B.m,B.v),p),p,p)],x))
w.push(B.aG)
v=A.bb(8)
u=A.en(B.t9,1)
w.push(A.ac(p,A.bc(A.a([A.fu(H.abz,B.ny,p,20),E.ca,A.cf(A.U("\u5728Web\u7248\u672c\u4e2d\uff0c\u6587\u4ef6\u5c06\u4e0b\u8f7d\u5230\u6d4f\u89c8\u5668\u7684\u9ed8\u8ba4\u4e0b\u8f7d\u6587\u4ef6\u5939",p,p,p,p,A.cB(p,p,B.ny,p,p,p,p,p,p,p,p,12,p,p,p,p,p,!0,p,p,p,p,p,p,p,p),p,p),1)],x),B.p,B.q,B.m,p),B.i,p,p,new A.b4(B.tc,p,u,v,p,p,p,B.F),p,p,p,p,E.cH,p,p,p))
if(q.z&&q.x!=null){v=A.a([A.bc(A.a([D.abS,E.ca,A.cf(A.U("\u6587\u4ef6\u5185\u5bb9\u9884\u89c8",p,p,p,p,A.w(d).p2.f,p,p),1),A.hg(p,p,p,p,B.lv,p,new C.bhT(q),p,p,p,"\u5173\u95ed\u9884\u89c8")],x),B.p,B.q,B.m,p),B.aP],x)
u=q.y
if(u!=null){t=A.bb(4)
v.push(A.ac(p,A.bc(A.a([D.ac0,B.bs,A.U("\u6587\u4ef6\u540d: "+u+".gcode",p,p,p,p,A.w(d).p2.z,p,p)],x),B.p,B.q,B.m,p),B.i,p,p,new A.b4(B.nJ,p,p,t,p,p,p,B.F),p,p,p,p,D.Bm,p,p,p))}v.push(B.aP)
u=A.bb(4)
v.push(A.ac(p,A.bc(A.a([D.ac8,B.bs,A.U("\u6587\u4ef6\u5927\u5c0f: "+q.aIm(q.x.length),p,p,p,p,A.w(d).p2.z,p,p)],x),B.p,B.q,B.m,p),B.i,p,p,new A.b4(B.nJ,p,p,u,p,p,p,B.F),p,p,p,p,D.Bm,p,p,p))
v.push(B.aG)
u=A.en(B.dV,1)
t=A.bb(8)
s=A.bb(8)
r=q.x
r.toString
v.push(A.ac(p,A.jA(s,new C.Xk(r,p),B.aY),B.i,p,p,new A.b4(p,p,u,t,p,p,p,B.F),p,400,p,p,p,p,p,p))
B.b.G(w,A.a([B.aG,G.aBI(new A.aY(B.d8,A.aX(v,B.aa,B.q,B.m,B.v),p),p,p)],x))}x=A.aX(w,B.cF,B.q,B.m,B.v)}}return A.lZ(o,p,A.ac(p,x,B.i,p,p,p,p,p,p,p,B.d8,p,p,p),p,p)},
aIm(d){if(d<1024)return""+d+" B"
else if(d<1048576)return B.d.aJ(d/1024,1)+" KB"
else return B.d.aJ(d/1048576,1)+" MB"}}
C.Xk.prototype={
Z(){var x=A.uG(0,null,null)
return new C.Xl(x,new A.pM(B.qO,$.aO()),A.a([],y.r))}}
C.Xl.prototype={
ak(){this.aw()},
n(){this.d.n()
var x=this.e
x.dx$=$.aO()
x.db$=0
this.au()},
aSW(d){this.M(new C.bj8(this,d))},
aQM(){var x=this,w=x.r
if(w.length!==0){x.M(new C.bj7(x))
x.aeH(w[x.w])}},
aTr(){var x=this,w=x.r
if(w.length!==0){x.M(new C.bj9(x))
x.aeH(w[x.w])}},
aeH(d){this.d.iE(d*20,B.hw,B.bA)},
D(d){var x=this,w=null,v=A.a(x.a.c.split("\n"),y.x),u=y.u,t=A.a([D.ac6,E.ca,A.cf(F.ahQ(!0,!1,x.e,w,w,D.acU,w,w,w,w,w,w,1,!1,x.gaSV(),w,w,w,w,B.bt,w),1)],u)
if(x.f.length!==0)B.b.G(t,A.a([A.U(""+(x.w+1)+"/"+x.r.length,w,w,w,w,A.w(d).p2.Q,w,w),E.ca,A.hg(w,w,w,w,D.aca,w,x.gaTq(),w,w,w,"\u4e0a\u4e00\u4e2a"),A.hg(w,w,w,w,D.abR,w,x.gaQL(),w,w,w,"\u4e0b\u4e00\u4e2a")],u))
return A.aX(A.a([A.ac(w,A.bc(t,B.p,B.q,B.m,w),B.i,w,w,new A.b4(B.nK,w,new A.dV(B.B,B.B,new A.bd(B.dV,1,B.N,-1),B.B),w,w,w,w,B.F),w,w,w,w,B.fH,w,w,w),A.cf(G.c5T(x.d,new C.bja(x,v),v.length,B.fH,w,!1),1)],u),B.p,B.q,B.m,B.v)}}
var z=a.updateTypes(["~()","@()","~(f)"])
C.bhM.prototype={
$0(){var x=this.a
x.d=x.f=!1},
$S:0}
C.bhN.prototype={
$1(d){var x
A.j(B.k,"_statusSubscription status "+d.b,!0)
if(d===B.cU){x=this.a.Q
if(x!=null)x.L(0)
$.ar().$1$0(y.a).aN("Connect success")}if(d===B.bz||d===B.dZ){A.j(B.h,"connectDevice, connect error",!0)
x=this.a.Q
if(x!=null)x.L(0)}},
$S:79}
C.bhO.prototype={
$0(){this.a.f=!1},
$S:0}
C.bhP.prototype={
$0(){var x=this.a
x.f=!1
x.d=!0
x.e=J.t(this.b)},
$S:0}
C.bhG.prototype={
$0(){var x=this.a
x.r=!0
x.w=0
x.z=!1},
$S:0}
C.bhH.prototype={
$0(){this.a.w=0.5},
$S:0}
C.bhI.prototype={
$0(){var x=this.a
x.x=B.ag.er(0,this.b)
x.y=this.c},
$S:0}
C.bhJ.prototype={
$0(){this.a.w=0.8},
$S:0}
C.bhK.prototype={
$0(){var x=this.a
x.w=1
x.z=!0},
$S:0}
C.bhL.prototype={
$0(){var x=this.a
x.r=!1
x.w=0},
$S:0}
C.bhR.prototype={
$0(){return this.a.DQ(!1)},
$S:0}
C.bhS.prototype={
$0(){return this.a.DQ(!0)},
$S:0}
C.bhT.prototype={
$0(){var x=this.a
x.M(new C.bhQ(x))},
$S:0}
C.bhQ.prototype={
$0(){var x=this.a
x.z=!1
x.y=x.x=null},
$S:0}
C.bj8.prototype={
$0(){var x,w,v,u=this.a,t=u.f=this.b,s=u.r
B.b.T(s)
u.w=-1
if(t.length!==0){x=u.a.c.split("\n")
for(w=0;w<x.length;++w){v=x[w].toLowerCase()
if(A.iB(v,t.toLowerCase(),0))s.push(w)}if(s.length!==0)u.w=0}},
$S:0}
C.bj7.prototype={
$0(){var x=this.a
x.w=B.e.ai(x.w+1,x.r.length)},
$S:0}
C.bj9.prototype={
$0(){var x=this.a,w=x.w
x.w=w<=0?x.r.length-1:w-1},
$S:0}
C.bja.prototype={
$2(d,e){var x=null,w="monospace",v=this.b[e],u=this.a,t=u.r,s=B.b.p(t,e),r=e===t[u.w]
if(r)u=E.Ak
else u=s?E.Al:x
t=A.ac(x,A.U(""+(e+1),x,x,x,x,A.cB(x,x,B.eX,x,x,x,x,x,w,x,x,12,x,x,x,x,x,!0,x,x,x,x,x,x,x,x),B.kv,x),B.i,x,x,x,x,x,x,x,H.Bj,x,x,50)
return A.ac(x,A.bc(A.a([t,A.cf(A.U(v,x,x,x,x,A.cB(x,x,r?B.A:B.dU,x,x,x,x,x,w,x,x,12,x,x,x,x,x,!0,x,x,x,x,x,x,x,x),x,x),1)],y.u),B.aa,B.q,B.m,x),B.i,x,x,new A.b4(u,x,x,x,x,x,x,B.F),x,x,x,x,D.a9C,x,x,x)},
$S:336};(function installTearOffs(){var x=a._instance_0u,w=a._instance_1u
x(C.WZ.prototype,"gaH0","zx",1)
var v
w(v=C.Xl.prototype,"gaSV","aSW",2)
x(v,"gaQL","aQM",0)
x(v,"gaTq","aTr",0)})();(function inheritance(){var x=a.inheritMany,w=a.inherit
x(A.a0,[C.wn,C.Xk])
x(A.a6,[C.WZ,C.Xl])
x(A.mt,[C.bhM,C.bhO,C.bhP,C.bhG,C.bhH,C.bhI,C.bhJ,C.bhK,C.bhL,C.bhR,C.bhS,C.bhT,C.bhQ,C.bj8,C.bj7,C.bj9])
w(C.bhN,A.jf)
w(C.bja,A.nH)})()
A.np(b.typeUniverse,JSON.parse('{"Xk":{"a0":[],"q":[],"r":[]},"wn":{"a0":[],"q":[],"r":[]},"WZ":{"a6":["wn"]},"Xl":{"a6":["Xk"]}}'))
var y={e:A.aa("kF<J>"),a:A.aa("oP"),x:A.aa("E<f>"),u:A.aa("E<q>"),r:A.aa("E<i>"),w:A.aa("f"),v:A.aa("vn"),b:A.aa("@"),q:A.aa("~")};(function constants(){D.Bl=new A.au(0,16,0,16)
D.a9C=new A.au(0,2,0,2)
D.Bm=new A.au(12,6,12,6)
D.abR=new A.dr(I.oy,20,null,null,null)
D.abq=new A.d2(57791,"MaterialIcons",null,!1)
D.abS=new A.dr(D.abq,null,B.fc,null,null)
D.abZ=new A.dr(B.ox,64,B.md,null,null)
D.abv=new A.d2(57965,"MaterialIcons",null,!1)
D.ac0=new A.dr(D.abv,16,null,null,null)
D.ac6=new A.dr(B.abD,20,null,null,null)
D.abs=new A.d2(57857,"MaterialIcons",null,!1)
D.Ci=new A.dr(D.abs,null,null,null,null)
D.abE=new A.d2(58889,"MaterialIcons",null,!1)
D.ac8=new A.dr(D.abE,16,null,null,null)
D.aca=new A.dr(H.Cc,20,null,null,null)
D.acU=new F.AX(null,null,null,null,null,null,null,null,null,null,"\u641c\u7d22\u5185\u5bb9...",null,null,null,null,null,null,null,null,null,null,!0,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,E.hn,!0,null,null,null)
D.Xh=new A.at(20,20,I.l0,null)
D.baK=new A.f6("\u4e0b\u8f7d\u6587\u4ef6",null,null,null,null,null,null,null,null,null)})()};
((a,b)=>{a[b]=a.current
a.eventLog.push({p:"main.dart.js_16",e:"endPart",h:b})})($__dart_deferred_initializers__,"WtwaQ7xjBdlCky/mfPEVypJ3FnY=");