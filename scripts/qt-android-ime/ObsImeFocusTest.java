import com.obsproject.studio.ObsImeFocus;

public class ObsImeFocusTest {
    public static void main(String[] args) {
        long initial = ObsImeFocus.generation();
        assert ObsImeFocus.shouldSkip(true, true, true, initial, initial, initial, true);
        assert !ObsImeFocus.shouldSkip(false, true, true, initial, initial, initial, true); // hidden
        assert !ObsImeFocus.shouldSkip(true, false, true, initial, initial, initial, true);
        assert !ObsImeFocus.shouldSkip(true, true, false, initial, initial, initial, true);
        assert !ObsImeFocus.shouldSkip(true, true, true, initial, initial, -1, true); // first show
        assert !ObsImeFocus.shouldSkip(true, true, true, initial, initial, initial, false);
        ObsImeFocus.focusChanged();
        long next = ObsImeFocus.generation();
        assert next != initial;
        assert !ObsImeFocus.shouldSkip(true, true, true, next, next, initial, true); // new editor
        assert !ObsImeFocus.shouldSkip(true, true, true, initial, next, initial, true); // queued old request
        assert ObsImeFocus.shouldSkip(true, true, true, next, next, next, true);
        System.out.println("IME duplicate-show policy passed");
    }
}
