// android.app.FragmentBreadCrumbs -- the row of titles ("Settings > Display")
// that follows an activity's fragment back stack; a tap on a title pops back
// to it. Deprecated since API 21 but still in the platform, and
// PreferenceActivity-era code names its click listener. Mirrors
// frameworks/base/core/java/android/app/FragmentBreadCrumbs.java.
//
// AOSP inflates each title from an internal layout
// (fragment_bread_crumb_item); the platform resources here do not carry it, so
// a title is a plain TextView and the separator between two is a "›" one. The
// state, the listener calls and what a tap pops are AOSP's.
package android.app;

import android.content.Context;
import android.util.AttributeSet;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.TextView;

@Deprecated
public class FragmentBreadCrumbs extends ViewGroup implements FragmentManager.OnBackStackChangedListener {

    /** Told of a tap on a title before the default (pop the back stack) happens. */
    public interface OnBreadCrumbClickListener {
        /**
         * @param backStack the entry of the tapped title, or null for the first one
         * @param flags the flags of the pop that would follow
         * @return true to handle the tap and stop the default
         */
        boolean onBreadCrumbClick(FragmentManager.BackStackEntry backStack, int flags);
    }

    /** AOSP makes its fixed crumbs (the top title, the parent title) as
     *  BackStackRecords with only their titles set; this is that much of one. */
    private static final class Crumb implements FragmentManager.BackStackEntry {
        private final CharSequence mTitle;
        private final CharSequence mShortTitle;

        Crumb(CharSequence title, CharSequence shortTitle) {
            mTitle = title;
            mShortTitle = shortTitle;
        }

        @Override public int getId() { return -1; }
        @Override public String getName() { return null; }
        @Override public int getBreadCrumbTitleRes() { return 0; }
        @Override public int getBreadCrumbShortTitleRes() { return 0; }
        @Override public CharSequence getBreadCrumbTitle() { return mTitle; }
        @Override public CharSequence getBreadCrumbShortTitle() { return mShortTitle; }
    }

    private static final int DEFAULT_GRAVITY = Gravity.START | Gravity.CENTER_VERTICAL;

    private Activity mActivity;
    private LinearLayout mContainer;
    private int mMaxVisible = -1;
    private int mGravity = DEFAULT_GRAVITY;

    private FragmentManager.BackStackEntry mTopEntry;
    private FragmentManager.BackStackEntry mParentEntry;
    private View.OnClickListener mParentClickListener;
    private OnBreadCrumbClickListener mOnBreadCrumbClickListener;

    private final View.OnClickListener mOnClickListener = new View.OnClickListener() {
        @Override public void onClick(View v) {
            if (!(v.getTag() instanceof FragmentManager.BackStackEntry)) return;
            FragmentManager.BackStackEntry bse = (FragmentManager.BackStackEntry) v.getTag();
            if (bse == mParentEntry) {
                if (mParentClickListener != null) mParentClickListener.onClick(v);
                return;
            }
            if (mOnBreadCrumbClickListener != null
                    && mOnBreadCrumbClickListener.onBreadCrumbClick(bse == mTopEntry ? null : bse, 0)) {
                return;
            }
            if (mActivity == null) return;
            if (bse == mTopEntry) {
                // Pop everything off the back stack.
                mActivity.getFragmentManager().popBackStack();
            } else {
                mActivity.getFragmentManager().popBackStack(bse.getId(), 0);
            }
        }
    };

    public FragmentBreadCrumbs(Context context) {
        this(context, null);
    }

    public FragmentBreadCrumbs(Context context, AttributeSet attrs) {
        this(context, attrs, 0);
    }

    public FragmentBreadCrumbs(Context context, AttributeSet attrs, int defStyleAttr) {
        this(context, attrs, defStyleAttr, 0);
    }

    public FragmentBreadCrumbs(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
        super(context, attrs, defStyleAttr, defStyleRes);
    }

    /** Attaches the crumbs to their activity: from here on they follow its back stack. */
    public void setActivity(Activity a) {
        mActivity = a;
        mContainer = new LinearLayout(a);
        mContainer.setOrientation(LinearLayout.HORIZONTAL);
        mContainer.setGravity(mGravity);
        addView(mContainer);
        a.getFragmentManager().addOnBackStackChangedListener(this);
        updateCrumbs();
    }

    /** The most titles to show at once; the earlier ones are hidden. */
    public void setMaxVisible(int visibleCrumbs) {
        if (visibleCrumbs < 1) {
            throw new IllegalArgumentException("visibleCrumbs must be greater than zero");
        }
        mMaxVisible = visibleCrumbs;
    }

    /** A title shown before all others, with its own click listener; null title removes it. */
    public void setParentTitle(CharSequence title, CharSequence shortTitle, View.OnClickListener listener) {
        mParentEntry = createBackStackEntry(title, shortTitle);
        mParentClickListener = listener;
        updateCrumbs();
    }

    public void setOnBreadCrumbClickListener(OnBreadCrumbClickListener listener) {
        mOnBreadCrumbClickListener = listener;
    }

    private FragmentManager.BackStackEntry createBackStackEntry(CharSequence title, CharSequence shortTitle) {
        if (title == null) return null;
        return new Crumb(title, shortTitle);
    }

    /** The title of the screen under the whole back stack. */
    public void setTitle(CharSequence title, CharSequence shortTitle) {
        mTopEntry = createBackStackEntry(title, shortTitle);
        updateCrumbs();
    }

    @Override protected void onLayout(boolean changed, int l, int t, int r, int b) {
        // Eventually we should implement our own layout of the views, rather than relying on
        // a single linear layout (AOSP's own note; it lays out its one container the same way).
        final int childCount = getChildCount();
        if (childCount == 0) return;
        final View child = getChildAt(0);
        final int childTop = getPaddingTop();
        final int childBottom = getPaddingTop() + child.getMeasuredHeight() - getPaddingBottom();
        int childLeft;
        int childRight;
        final int layoutDirection = getLayoutDirection();
        final int horizontalGravity = mGravity & Gravity.RELATIVE_HORIZONTAL_GRAVITY_MASK;
        switch (Gravity.getAbsoluteGravity(horizontalGravity, layoutDirection)) {
            case Gravity.RIGHT:
                childRight = getWidth() - getPaddingRight();
                childLeft = childRight - child.getMeasuredWidth();
                break;
            case Gravity.CENTER_HORIZONTAL:
                childLeft = getPaddingLeft() + (getWidth() - child.getMeasuredWidth()) / 2;
                childRight = childLeft + child.getMeasuredWidth();
                break;
            case Gravity.LEFT:
            default:
                childLeft = getPaddingLeft();
                childRight = childLeft + child.getMeasuredWidth();
                break;
        }
        if (childLeft < getPaddingLeft()) childLeft = getPaddingLeft();
        if (childRight > getWidth() - getPaddingRight()) childRight = getWidth() - getPaddingRight();
        child.layout(childLeft, childTop, childRight, childBottom);
    }

    @Override protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        final int count = getChildCount();
        int maxHeight = 0;
        int maxWidth = 0;
        int measuredChildState = 0;
        for (int i = 0; i < count; i++) {
            final View child = getChildAt(i);
            if (child.getVisibility() != GONE) {
                measureChild(child, widthMeasureSpec, heightMeasureSpec);
                maxWidth = Math.max(maxWidth, child.getMeasuredWidth());
                maxHeight = Math.max(maxHeight, child.getMeasuredHeight());
                measuredChildState = combineMeasuredStates(measuredChildState, child.getMeasuredState());
            }
        }
        maxWidth += getPaddingLeft() + getPaddingRight();
        maxHeight += getPaddingTop() + getPaddingBottom();
        maxHeight = Math.max(maxHeight, getSuggestedMinimumHeight());
        maxWidth = Math.max(maxWidth, getSuggestedMinimumWidth());
        setMeasuredDimension(resolveSizeAndState(maxWidth, widthMeasureSpec, measuredChildState),
                resolveSizeAndState(maxHeight, heightMeasureSpec, measuredChildState << MEASURED_HEIGHT_STATE_SHIFT));
    }

    @Override public void onBackStackChanged() {
        updateCrumbs();
    }

    /** How many fixed titles (top, parent) come before the back stack's own. */
    private int getPreEntryCount() {
        return (mTopEntry != null ? 1 : 0) + (mParentEntry != null ? 1 : 0);
    }

    private FragmentManager.BackStackEntry getPreEntry(int index) {
        // If there's a parent entry, then return that for zero'th item, else top entry.
        if (mParentEntry != null) {
            return index == 0 ? mParentEntry : mTopEntry;
        }
        return mTopEntry;
    }

    void updateCrumbs() {
        if (mActivity == null || mContainer == null) return;
        FragmentManager fm = mActivity.getFragmentManager();
        int numEntries = fm.getBackStackEntryCount();
        int numPreEntries = getPreEntryCount();
        int numViews = mContainer.getChildCount();
        for (int i = 0; i < numEntries + numPreEntries; i++) {
            FragmentManager.BackStackEntry bse = i < numPreEntries
                    ? getPreEntry(i) : fm.getBackStackEntryAt(i - numPreEntries);
            if (i < numViews) {
                View v = mContainer.getChildAt(i);
                Object tag = v.getTag();
                if (tag != bse) {
                    // The stack changed under this title: drop it and all after it.
                    for (int j = i; j < numViews; j++) {
                        mContainer.removeViewAt(i);
                    }
                    numViews = i;
                }
            }
            if (i >= numViews) {
                TextView text = new TextView(mActivity);
                CharSequence title = bse.getBreadCrumbTitle();
                text.setText(i == 0 ? title : " › " + title);
                text.setTag(bse);
                text.setOnClickListener(mOnClickListener);
                mContainer.addView(text);
            }
        }
        int viewI = numEntries + numPreEntries;
        numViews = mContainer.getChildCount();
        while (numViews > viewI) {
            mContainer.removeViewAt(numViews - 1);
            numViews--;
        }
        // Adjust the visibility and availability of the bread crumbs.
        for (int i = 0; i < numViews; i++) {
            final View child = mContainer.getChildAt(i);
            // Disable the last one: it is the screen the user is on.
            child.setEnabled(i < numViews - 1);
            if (mMaxVisible > 0) {
                // Make only the last mMaxVisible crumbs visible.
                child.setVisibility(i < numViews - mMaxVisible ? View.GONE : View.VISIBLE);
            }
        }
    }
}
