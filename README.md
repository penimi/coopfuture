# coopfuture

Introduction
============

Coopfuture provides spawn/await functionality on top of the boost::context
library. In addition it provides a Future primitive (on top of which spawn'ed
tasks runs) which works in a similar fashion to the Python asyncio Future type.
This extension is intended for use with boost::asio, which can be used to
provide the asynchronous IO functionality.


Usage
=====

Coopfuture provides two basic classes, a Scheduler and a Future. A Future is
always created with a link to a specific scheduler. The Future object provided
is an analog to the Future object from the Python asyncio library. It provides
the promise of a result at some point in the future, to be fulfilled at one
single point. This parallel task is waited on asynchronously until the actual
result of the Future is directly required, at which point, execution is
suspended and the scheduler is invoked in an attempt to fulfill the Future
through other scheduled calls.


