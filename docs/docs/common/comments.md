---
id: comments
title: Comments
sidebar_position: 5
---

# Comments

In general, programmers try to keep code easy to read, but there are times when you need
extra explanation. In these cases programmers leave comments in the source code that Lesma
will ignore, but others will read.

Comments start after `#`. Here's a simple comment

```python
# My first comment!
```

Everything after `#` will be considered a comment until the end of the line. To span multiple
lines you need to add `#` on each line.

```python
# This
# is
# a
# multiline
# comment
```

You'll usually see comments either after the line it's about, or, more commonly, above it.

```python
# approximate π value
pi = 3.14

delta = 0.001 # My δ symbol showing the difference
```