#include <glretrace.hpp>
#include <os_thread.hpp>

namespace glretrace {

class ThreadState {
	Context *current_context;
	glws::Drawable *current_drawable;

public:
	glws::Drawable *get_current_drawable(void)
	{
		return current_drawable;
	}

	void set_current_drawable(glws::Drawable *drawable)
	{
		current_drawable = drawable;
	}

	Context *get_current_context(void)
	{
		return current_context;
	}

	void set_current_context(Context *context)
	{
		current_context = context;
	}

	ThreadState(void) : current_context(NULL), current_drawable(NULL) { }
};

static os::thread_specific_ptr<class ThreadState> thread_state;

static ThreadState *get_thread_state(void)
{
	ThreadState *ts = thread_state.get();

	if (!ts) {
		ts = new ThreadState;
		thread_state.reset(ts);
	}

	return ts;
}

Context *getCurrentContext(void)
{
	return get_thread_state()->get_current_context();
}

void setCurrentContext(Context *context)
{
	get_thread_state()->set_current_context(context);
}

glws::Drawable *getCurrentDrawable(void)
{
	return get_thread_state()->get_current_drawable();
}

void setCurrentDrawable(glws::Drawable *drawable)
{
	get_thread_state()->set_current_drawable(drawable);
}

}
