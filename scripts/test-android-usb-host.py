#!/usr/bin/env python3
"""Exercise both production USB-host implementations with deterministic Android stubs.

Set JAVAC and JAVA to a JDK 17+ toolchain. No tablet is required.
"""
import pathlib, subprocess, tempfile, sys, os

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCES = [ROOT / 'android-shell/android/java/org/qtproject/example/obs_shell/ObsUsbHost.java', ROOT / 'obs-studio/frontend/cmake/android/java/com/obsproject/studio/ObsUsbHost.java']
STUBS = {
'android/app/PendingIntent.java': '''package android.app; import android.content.*; public class PendingIntent { public static final int FLAG_IMMUTABLE=1; public static PendingIntent getBroadcast(Context c,int i,Intent n,int f){return new PendingIntent();} }''',
'android/content/BroadcastReceiver.java': '''package android.content; public abstract class BroadcastReceiver { public abstract void onReceive(Context c,Intent i); }''',
'android/content/Context.java': '''package android.content; import android.hardware.usb.*; public class Context { public static final String USB_SERVICE="usb"; public static final int RECEIVER_NOT_EXPORTED=1; public final UsbManager manager=new UsbManager(); public BroadcastReceiver receiver; public Context getApplicationContext(){return this;} public Object getSystemService(String s){return manager;} public void registerReceiver(BroadcastReceiver r,IntentFilter f,int flags){receiver=r;} public void sendBroadcast(Intent i){} public String getPackageName(){return "test";} }''',
'android/content/Intent.java': '''package android.content; public class Intent { String action; Object device; public Intent(String a){action=a;} public Intent setPackage(String p){return this;} public String getAction(){return action;} public boolean getBooleanExtra(String s,boolean b){return b;} public Intent device(Object d){device=d;return this;} public <T> T getParcelableExtra(String s,Class<T> c){return c.cast(device);} @SuppressWarnings("unchecked") public <T> T getParcelableExtra(String s){return (T)device;} }''',
'android/content/IntentFilter.java': '''package android.content; public class IntentFilter { public void addAction(String s){} }''',
'android/hardware/usb/UsbDevice.java': '''package android.hardware.usb; public class UsbDevice { public final String name; public UsbDevice(String n){name=n;} public String getDeviceName(){return name;} public String getProductName(){return "test";} public int getVendorId(){return 1;} public int getProductId(){return 2;} }''',
'android/hardware/usb/UsbDeviceConnection.java': '''package android.hardware.usb; public class UsbDeviceConnection { public final int fd; public boolean closed; public UsbDeviceConnection(int f){fd=f;} public int getFileDescriptor(){return fd;} public void close(){closed=true;} }''',
'android/hardware/usb/UsbManager.java': '''package android.hardware.usb; import java.util.*; import android.app.PendingIntent; public class UsbManager { public static final String ACTION_USB_DEVICE_ATTACHED="attach", ACTION_USB_DEVICE_DETACHED="detach", EXTRA_DEVICE="device", EXTRA_PERMISSION_GRANTED="permission"; public HashMap<String,UsbDevice> devices=new HashMap<>(); public ArrayList<UsbDeviceConnection> opened=new ArrayList<>(); public HashMap<String,UsbDevice> getDeviceList(){return devices;} public boolean hasPermission(UsbDevice d){return true;} public void requestPermission(UsbDevice d,PendingIntent p){} public UsbDeviceConnection openDevice(UsbDevice d){UsbDeviceConnection c=new UsbDeviceConnection(10+opened.size()); opened.add(c); return c;} }''',
'android/os/Build.java': '''package android.os; public class Build { public static class VERSION { public static int SDK_INT=35; } }''',
'android/os/ParcelFileDescriptor.java': '''package android.os; import java.io.*; import java.util.*; public class ParcelFileDescriptor { static int next=100; public static boolean fail; public static HashSet<Integer> live=new HashSet<>(); int fd; ParcelFileDescriptor(int f){fd=f;} public static ParcelFileDescriptor fromFd(int f) throws IOException {if(fail) throw new IOException(); int fd=next++; live.add(fd); return new ParcelFileDescriptor(fd);} public int getFd(){return fd;} public void close() throws IOException {live.remove(fd);} }''',
'android/util/Log.java': '''package android.util; public class Log { public static int i(String t,String m){return 0;} public static int w(String t,String m){return 0;} public static int w(String t,String m,Throwable e){return 0;} public static int e(String t,String m){return 0;} public static int e(String t,String m,Throwable e){return 0;} }''',
'Test.java': '''import android.content.*; import android.hardware.usb.*; import android.os.*; import java.lang.reflect.*; import java.util.concurrent.*;
public class Test {
 static void check(boolean b,String s){if(!b) throw new AssertionError(s);}
 static Object call(Class<?> c,String name,Class<?>[] types,Object... args) throws Exception {return c.getMethod(name,types).invoke(null,args);}
 static int open(Class<?> c,String n) throws Exception{return (int)call(c,"openFd",new Class[]{String.class},n);}
 static void close(Class<?> c,int fd) throws Exception{call(c,"closeFd",new Class[]{int.class},fd);}
 static void run(String name) throws Exception {
  Class<?> c=Class.forName(name); Context ctx=new Context(); call(c,"install",new Class[]{Context.class},ctx);
  UsbDevice d=new UsbDevice("camera"); ctx.manager.devices.put(d.name,d);
  ExecutorService pool=Executors.newFixedThreadPool(2); CountDownLatch latch=new CountDownLatch(1);
  Future<Integer> x=pool.submit(()->{latch.await();return open(c,"camera");}); Future<Integer> y=pool.submit(()->{latch.await();return open(c,"camera");}); latch.countDown(); int a=x.get(),b=y.get(); pool.shutdown(); check((a>=0) != (b>=0),"exclusive concurrent open"); int old=Math.max(a,b); UsbDeviceConnection first=ctx.manager.opened.get(0); check(!first.closed,"busy closed active connection");
  ctx.manager.devices.remove(d.name); ctx.receiver.onReceive(ctx,new Intent(UsbManager.ACTION_USB_DEVICE_DETACHED).device(d)); check(first.closed,"detach closes connection"); check(ParcelFileDescriptor.live.contains(old),"detach retains loan");
  UsbDevice replacement=new UsbDevice("camera"); ctx.manager.devices.put(replacement.name,replacement); int fresh=open(c,"camera"); check(fresh>=0 && fresh!=old,"reattach opens distinct loan"); UsbDeviceConnection second=ctx.manager.opened.get(1); close(c,old); check(!second.closed && open(c,"camera")==-1,"old close leaves new loan"); close(c,old); check(!second.closed,"duplicate old close"); close(c,fresh); close(c,fresh); check(second.closed && !ParcelFileDescriptor.live.contains(fresh),"normal and duplicate close");
  ParcelFileDescriptor.fail=true; check(open(c,"camera")==-1,"dup failure"); check(ctx.manager.opened.get(2).closed,"dup failure closes original"); ParcelFileDescriptor.fail=false;
  int[] fds=new int[8]; for(int i=0;i<8;i++){String n="d"+i; ctx.manager.devices.put(n,new UsbDevice(n)); fds[i]=open(c,n); check(fds[i]>=0,"open under cap");} ctx.manager.devices.put("extra",new UsbDevice("extra")); check(open(c,"extra")==-1,"cap");
  UsbDevice detached=ctx.manager.devices.remove("d0"); ctx.receiver.onReceive(ctx,new Intent(UsbManager.ACTION_USB_DEVICE_DETACHED).device(detached)); check(open(c,"extra")==-1,"detached loan counts toward cap"); close(c,fds[0]); int extra=open(c,"extra"); check(extra>=0,"returned detached loan frees capacity"); close(c,extra); for(int i=1;i<fds.length;i++)close(c,fds[i]);
  check((int)call(c,"connectionCount",new Class[]{})==0,"all connections closed"); System.out.println(name+" OK");
 }
 public static void main(String[] a) throws Exception {run("org.qtproject.example.obs_shell.ObsUsbHost");run("com.obsproject.studio.ObsUsbHost");}
}'''
}
with tempfile.TemporaryDirectory(prefix='usb-host-test-') as tmp:
    tmp = pathlib.Path(tmp)
    for name, data in STUBS.items():
        path=tmp/name; path.parent.mkdir(parents=True, exist_ok=True); path.write_text(data, encoding='utf-8')
    javac=os.environ.get('JAVAC','javac'); java=os.environ.get('JAVA','java')
    files=[str(tmp/name) for name in STUBS]+[str(p) for p in SOURCES]
    subprocess.run([javac,'-encoding','UTF-8','-d',str(tmp/'classes')]+files,check=True)
    subprocess.run([java,'-cp',str(tmp/'classes'),'Test'],check=True)
