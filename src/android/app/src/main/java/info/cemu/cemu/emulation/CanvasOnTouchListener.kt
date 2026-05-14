package info.cemu.cemu.emulation

import android.annotation.SuppressLint
import android.view.MotionEvent
import android.view.View
import info.cemu.cemu.nativeinterface.NativeInput

class CanvasOnTouchListener(val isTV: Boolean) : View.OnTouchListener {
    private var currentPointerId: Int = -1

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouch(v: View, event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                if (currentPointerId != -1) return false
                val pointerIndex = event.actionIndex
                currentPointerId = event.getPointerId(pointerIndex)
                NativeInput.onTouchDown(
                    event.getX(pointerIndex).toInt(),
                    event.getY(pointerIndex).toInt(),
                    isTV,
                )
                return true
            }

            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> {
                val pointerIndex = event.actionIndex
                if (event.getPointerId(pointerIndex) != currentPointerId) return false
                currentPointerId = -1
                NativeInput.onTouchUp(
                    event.getX(pointerIndex).toInt(),
                    event.getY(pointerIndex).toInt(),
                    isTV,
                )
                return true
            }

            MotionEvent.ACTION_CANCEL -> {
                if (currentPointerId == -1) return false
                val pointerIndex = event.findPointerIndex(currentPointerId)
                val x = if (pointerIndex >= 0) event.getX(pointerIndex).toInt() else 0
                val y = if (pointerIndex >= 0) event.getY(pointerIndex).toInt() else 0
                currentPointerId = -1
                NativeInput.onTouchUp(x, y, isTV)
                return true
            }

            MotionEvent.ACTION_MOVE -> {
                if (currentPointerId == -1) return false
                val pointerIndex = event.findPointerIndex(currentPointerId)
                if (pointerIndex < 0) return false
                NativeInput.onTouchMove(
                    event.getX(pointerIndex).toInt(),
                    event.getY(pointerIndex).toInt(),
                    isTV,
                )
                return true
            }
        }
        return false
    }
}