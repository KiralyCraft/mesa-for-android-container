`Mesa <https://mesa3d.org>`_ - The 3D Graphics Library
======================================================

KGSL paced-Present compatibility
--------------------------------

The ``kgsl-present-wait-fence-paced-minimal`` branch is the Mesa side of a
standalone, bounded Present-pacing pair for Termux:X11.  Its runtime behaviour
is derived from Mesa commit
``dcb971f9cdaa69587f1e3c92e85915e421827239``, with unused future telemetry
scaffolding removed.

Use it with the Termux:X11 branch
`fix/present-vblank-pacing-minimal
<https://github.com/KiralyCraft/termux-x11/tree/fix/present-vblank-pacing-minimal>`_,
whose matching runtime checkpoint is
`cbf866f573253110accd641f72650bad438c220d
<https://github.com/KiralyCraft/termux-x11/commit/cbf866f573253110accd641f72650bad438c220d>`_.
Enable the policy explicitly with ``MESA_DRI3_PRESENT_MODE=paced``; synchronized
unpaced Present remains the default and fallback.

This pair retains PR96's native-fence-to-X-Sync bridge, bounded production and
submission, MSC-based pacing, API-33 Choreographer timestamps, API-24 fallback,
and server-reset recovery.  It intentionally excludes measured GMEM/SYSMEM
training, consumer-owned allocation, actual-presentation feedback, custom
deadline/opportunity transport, and SurfaceControl direct presentation.


Source
------

This repository lives at https://gitlab.freedesktop.org/mesa/mesa.
Other repositories are likely forks, and code found there is not supported.


Build & install
---------------

You can find more information in our documentation (`docs/install.rst
<https://docs.mesa3d.org/install.html>`_), but the recommended way is to use
Meson (`docs/meson.rst <https://docs.mesa3d.org/meson.html>`_):

.. code-block:: sh

  $ meson setup build
  $ ninja -C build/
  $ sudo ninja -C build/ install

Support
-------

Many Mesa devs hang on IRC; if you're not sure which channel is
appropriate, you should ask your question on `OFTC's #dri-devel
<irc://irc.oftc.net/dri-devel>`_, someone will redirect you if
necessary.
Remember that not everyone is in the same timezone as you, so it might
take a while before someone qualified sees your question.
To figure out who you're talking to, or which nick to ping for your
question, check out `Who's Who on IRC
<https://dri.freedesktop.org/wiki/WhosWho/>`_.

The next best option is to ask your question in an email to the
mailing lists: `mesa-dev\@lists.freedesktop.org
<https://lists.freedesktop.org/mailman/listinfo/mesa-dev>`_


Bug reports
-----------

If you think something isn't working properly, please file a bug report
(`docs/bugs.rst <https://docs.mesa3d.org/bugs.html>`_).


Contributing
------------

Contributions are welcome, and step-by-step instructions can be found in our
documentation (`docs/submittingpatches.rst
<https://docs.mesa3d.org/submittingpatches.html>`_).

Note that Mesa uses gitlab for patches submission, review and discussions.
