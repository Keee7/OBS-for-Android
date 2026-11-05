package com.obsproject.studio;

public final class ObsProjectionSessionTest {
    private static void check(boolean ok) { if (!ok) throw new AssertionError(); }
    private static void fails(Runnable action) {
        try { action.run(); } catch (IllegalStateException expected) { return; }
        throw new AssertionError("Expected a rejected session");
    }
    private static final class Display implements ObsProjectionSession.Display<Object> {
        int releases, width, height, density;
        Object surface;
        public void resize(int w, int h, int dpi) { width=w; height=h; density=dpi; }
        public void surface(Object s) { surface=s; }
        public void release() { releases++; surface=null; }
    }
    public static void main(String[] args) {
        int[] creates={0};
        Display target=new Display();
        ObsProjectionSession.Factory<Object> factory=(s,w,h,dpi) -> {
            creates[0]++; target.resize(w,h,dpi); target.surface(s); return target;
        };
        ObsProjectionSession<Object> session=new ObsProjectionSession<>(factory);
        Object first=new Object(), rotated=new Object();
        int[] fullDisplay={3200,2136,320,1};
        check(session.captureSize(fullDisplay)==fullDisplay);
        session.attach(first,1920,1080,240);
        check(creates[0]==1 && session.isAttached() && target.surface==first);
        check(session.contentResized(1200,1800));
        int[] appSize=session.captureSize(fullDisplay);
        check(appSize[0]==1200 && appSize[1]==1800 && appSize[2]==320 && appSize[3]==1);
        check(!session.contentResized(1200,1800) && !session.contentResized(0,1800));
        check(fullDisplay[0]==3200 && fullDisplay[1]==2136);
        session.detach();
        check(session.hasDisplay() && !session.isAttached() && target.surface==null && target.releases==0);
        check(session.captureSize(fullDisplay)[0]==1200);
        session.attach(rotated,1080,1920,320);
        session.detach(); session.attach(first,1616,1080,320);
        check(creates[0]==1 && target.width==1616 && target.height==1080 && target.density==320);
        // Resizing the destination must not replace the system-reported source size.
        check(session.captureSize(fullDisplay)[1]==1800);
        check(session.contentResized(1800,1200));
        check(session.captureSize(fullDisplay)[0]==1800 && session.captureSize(fullDisplay)[1]==1200);
        session.release(); session.release(); session.detach();
        check(target.releases==1 && !session.hasDisplay() && !session.isAttached());
        check(!session.contentResized(640,480) && session.captureSize(fullDisplay)==fullDisplay);
        fails(() -> session.attach(first,1920,1080,240));
        check(creates[0]==1);
        ObsProjectionSession<Object> fresh=new ObsProjectionSession<>(factory);
        check(fresh.captureSize(fullDisplay)==fullDisplay);
        fresh.attach(first,1920,1080,240);
        check(creates[0]==2); fresh.release();

        int[] attempts={0};
        ObsProjectionSession<Object> failed=new ObsProjectionSession<>((s,w,h,dpi) -> {
            attempts[0]++; throw new IllegalStateException("Token consumed by system");
        });
        fails(() -> failed.attach(first,1920,1080,240));
        fails(() -> failed.attach(first,1920,1080,240));
        check(attempts[0]==1 && !failed.hasDisplay()); failed.release();
        ObsProjectionSession<Object> missing=new ObsProjectionSession<>((s,w,h,dpi) -> null);
        fails(() -> missing.attach(first,1920,1080,240));
        fails(() -> missing.attach(first,1920,1080,240)); missing.release();

        ObsProjectionSession<Object> invalid=new ObsProjectionSession<>(factory);
        fails(() -> invalid.attach(first,0,1080,240));
        invalid.attach(first,1920,1080,240); invalid.release();
        check(creates[0]==3);
        System.out.println("PASS: display reuse, app content bounds, pause/resume, rotation, release, fresh consent, failed-token and null-display handling");
    }
}
