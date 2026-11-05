"""Run the production preview rectangle updater against Android layout stubs."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
source = (root / 'obs-studio/frontend/cmake/android/java/com/obsproject/studio/ObsDisplayHost.java').read_text(encoding='utf-8')
start = source.index('    private static boolean applyRequestedRect(')
end = source.index('    public static boolean updateRect(', start)
method = source[start:end]
test = r'''
class ObsDisplayHost {
 static final String TAG="test";
 static SurfaceView view;
 static int reqX,reqY,reqW,reqH,onScreenX,onScreenY,onScreenW,onScreenH;
 static class Log { static void i(String t,String s) {} }
 static class Gravity { static final int TOP=48, LEFT=3; }
 static class ViewGroup {
  int x,y,screenX,screenY,padX,padY,scrollX,scrollY;
  static class LayoutParams { int width,height; }
  void getLocationInWindow(int[] o) {o[0]=x;o[1]=y;}
  void getLocationOnScreen(int[] o) {o[0]=screenX+x;o[1]=screenY+y;}
  int getPaddingLeft(){return padX;} int getPaddingTop(){return padY;}
  int getScrollX(){return scrollX;} int getScrollY(){return scrollY;}
 }
 static class FrameLayout { static class LayoutParams extends ViewGroup.LayoutParams {int leftMargin,topMargin,gravity;} }
 static class SurfaceView {
  ViewGroup parent=new ViewGroup(); FrameLayout.LayoutParams lp=new FrameLayout.LayoutParams(); int layouts;
  Object getParent(){return parent;} ViewGroup.LayoutParams getLayoutParams(){return lp;}
  void setLayoutParams(FrameLayout.LayoutParams p){lp=p;layouts++;}
  int getWidth(){return lp.width;} int getHeight(){return lp.height;}
  void getLocationOnScreen(int[] o){o[0]=parent.screenX+parent.x+parent.padX-parent.scrollX+lp.leftMargin;o[1]=parent.screenY+parent.y+parent.padY-parent.scrollY+lp.topMargin;}
 }
 __METHOD__
 static void check(boolean b){if(!b)throw new AssertionError();}
 static void target(int x,int y,int w,int h){reqX=x;reqY=y;reqW=w;reqH=h;check(applyRequestedRect(view));check(onScreenX==view.parent.screenX+x&&onScreenY==view.parent.screenY+y);check(onScreenW==w&&onScreenH==h);}
 public static void main(String[] args){
  view=new SurfaceView();view.parent.y=85;
  target(983,85,2165,1228); check(view.lp.topMargin==0);
  target(983,200,2165,900);check(view.lp.topMargin==115);
  int n=view.layouts;target(983,200,2165,900);check(view.layouts==n);
  // Insets change while Qt's target is unchanged.
  view.parent.y=0;target(983,200,2165,900);check(view.lp.topMargin==200);
  // Dock/ancestor movement, padding and scrolling.
  view.parent.x=37;view.parent.y=91;view.parent.padX=8;view.parent.padY=12;
  view.parent.scrollX=4;view.parent.scrollY=6;target(350,10,930,525);
  // Actual Xiaomi floating-window evidence: Qt target is window-local,
  // while the Android parent has a nonzero physical-screen origin.
  view.parent.x=0;view.parent.y=0;view.parent.padX=0;view.parent.padY=0;
  view.parent.scrollX=0;view.parent.scrollY=0;
  view.parent.screenX=820;view.parent.screenY=251;
  target(678,100,1620,828);check(view.lp.leftMargin==678&&view.lp.topMargin==100);
  n=view.layouts;target(678,100,1620,828);check(view.layouts==n);
  // Moving a floating window must move its preview with the same window.
  view.parent.screenX=1040;view.parent.screenY=380;
  target(678,100,1620,828);check(view.layouts==n);
  target(678,100,1500,750);target(690,120,1500,750);
  // Return to fullscreen, then start a fresh preview directly in a small window.
  view.parent.screenX=0;view.parent.screenY=0;target(983,85,2165,1228);
  view=new SurfaceView();view.parent.screenX=820;view.parent.screenY=251;
  target(678,100,1620,828);check(view.lp.leftMargin==678&&view.lp.topMargin==100);
  check(!applyRequestedRect(new SurfaceView()));
  reqH=0;check(!applyRequestedRect(view));
  System.out.println("PASS: preview insets, resize, ancestor movement, window-space placement, floating-window move/resize/startup and fullscreen return");
 }
}
'''.replace('__METHOD__', method)
java = root.parent / 'build-tools/jdk-17.0.12/bin'
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp) / 'ObsDisplayHost.java'
    path.write_text(test, encoding='utf-8')
    subprocess.run([str(java / 'javac.exe'), '-encoding', 'UTF-8', str(path)], check=True)
    subprocess.run([str(java / 'java.exe'), '-ea', '-cp', tmp, 'ObsDisplayHost'], check=True)
