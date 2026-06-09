## A Software Architecture for Browser-based Programming Language Implementation 

Samuel Tan 

supervised by Prof. Martin Henz 

AY 2024/2025 

## **Abstract** 

The Source Academy is a computer-mediated learning environment for studying the structure and interpretation of computer programs. Students write and run their programs in their web browser, using sublanguages of JavaScript called Source, designed for the textbook Structure and Interpretation of Computer Programs, JavaScript Edition. The Source Academy has in recent years gradually started to support more languages, such as Scheme, Python, C, and Java [2]. However, Source Academy remains centered around Source itself, with the new languages having to be built around this limitation. This project aims to provide a standard and easily extensible interface for languages to interact with Source Academy’s frontend and other systems. A framework was created for this purpose, enabling the decoupling of other languages from Source. Furthermore, the execution of user programs is moved to a separate thread, so long-running programs do not impact the functionality of the user interface. 

## **Subject Descriptors** 

-  D.2.2: Design Tools and Techniques 

-  D.2.6: Programming Environments 

-  D.2.11: Software Architectures 

-  D.2.12: Interoperability 

-  D.2.13: Reusable Software 

## **Keywords** 

Framework development, large-scale refactoring, software integration, modular software 

## **Acknowledgement** 

I would like to express my heartfelt gratitude to my project supervisor, Prof. Martin Henz, for providing guidance throughout the project. His knowledge has helped with understanding the needs that the new framework would need to fulfil, and his timely feedback has allowed smooth and continuous progress on this large-scale refactoring project. His support with trialling the new framework in his course CS4215 Programming Language Implementation has been instrumental in the improvement of the documentation for the framework by pointing out unclear sections. 

I would also like to thank Prof. Ashish Dandekar, the main evaluator of this project, for the feedback he provided on the project. 

Finally, I would like to thank the Source Academy team (Kyriel Mortel Abad, Richard Dominick, Zhang Yao, Lee Yi) for answering questions about the design of various Source Academy systems as well as providing feedback to refine the design of the new framework. 

1 

## **Contents** 

|**Contents**|**Contents**|**Contents**||**2**|
|---|---|---|---|---|
|**1**|**Introduction**|||**4**|
||1.1|Background of the Source Academy|. . . . . . . . . . . . . . . . . . . . . . . . . . . . . .|4|
||1.2|Defnitions . . . . . . . . . . . . . . .|. . . . . . . . . . . . . . . . . . . . . . . . . . . . . .|5|
||1.3|Project Objectives . . . . . . . . . .|. . . . . . . . . . . . . . . . . . . . . . . . . . . . . .|5|
|||1.3.1<br>Supporting Implementation of|New Languages<br>. . . . . . . . . . . . . . . . . . . .|5|
|||1.3.2<br>Establishing a Common Interface to Interact with Modules . . . . . . . . . . . . .||6|
|||1.3.3<br>Lazy Loading . . . . . . . . .|. . . . . . . . . . . . . . . . . . . . . . . . . . . . . .|6|
|||1.3.4<br>Evaluating Programs using Web Workers<br>. . . . . . . . . . . . . . . . . . . . . . .||6|
|**2**|**Literature Review**|||**7**|
||2.1|Design Patterns . . . . . . . . . . . .|. . . . . . . . . . . . . . . . . . . . . . . . . . . . . .|7|
|||2.1.1<br>Observer Pattern . . . . . . .|. . . . . . . . . . . . . . . . . . . . . . . . . . . . . .|7|
|||2.1.2<br>Event Queue Pattern . . . . .|. . . . . . . . . . . . . . . . . . . . . . . . . . . . . .|7|
||2.2|Usage of the Source Academy in Teaching . . . . . . . . . . . . . . . . . . . . . . . . . . .||7|
|**3**|**Pre-design Preparations**|||**8**|
||3.1|Project Planning . . . . . . . . . . .|. . . . . . . . . . . . . . . . . . . . . . . . . . . . . .|8|
||3.2|Web Worker Proof of Concept . . . .|. . . . . . . . . . . . . . . . . . . . . . . . . . . . . .|8|
||3.3|Defning Terms . . . . . . . . . . . .|. . . . . . . . . . . . . . . . . . . . . . . . . . . . . .|9|
|**4**|**Architecting a new framework**|||**10**|
||4.1|Basic Design Ideas . . . . . . . . . .|. . . . . . . . . . . . . . . . . . . . . . . . . . . . . .|10|
||4.2|The First Attempt . . . . . . . . . .|. . . . . . . . . . . . . . . . . . . . . . . . . . . . . .|10|
||4.3|The Second Attempt . . . . . . . . .|. . . . . . . . . . . . . . . . . . . . . . . . . . . . . .|11|
||4.4|The Third Attempt . . . . . . . . . .|. . . . . . . . . . . . . . . . . . . . . . . . . . . . . .|11|
||4.5|Module Support<br>. . . . . . . . . . .|. . . . . . . . . . . . . . . . . . . . . . . . . . . . . .|11|
||4.6|The Final Design . . . . . . . . . . .|. . . . . . . . . . . . . . . . . . . . . . . . . . . . . .|12|
|||4.6.1<br>Overview<br>. . . . . . . . . . .|. . . . . . . . . . . . . . . . . . . . . . . . . . . . . .|12|
|||4.6.2<br>Flexibility of the Conductor Framework . . . . . . . . . . . . . . . . . . . . . . . .||13|
|||4.6.3<br>The Temporary Module API|. . . . . . . . . . . . . . . . . . . . . . . . . . . . . .|13|
||4.7|Framework Trials . . . . . . . . . . .|. . . . . . . . . . . . . . . . . . . . . . . . . . . . . .|14|
|**5**|**Integration into the Source Academy**|||**15**|
||5.1|Languages Directory . . . . . . . . .|. . . . . . . . . . . . . . . . . . . . . . . . . . . . . .|15|
||5.2|Migrating the Source Language onto the API . . . . . . . . . . . . . . . . . . . . . . . . .||16|
||5.3|Migrating Visualisation Tools . . . .|. . . . . . . . . . . . . . . . . . . . . . . . . . . . . .|16|
||5.4|Plugins Directory . . . . . . . . . . .|. . . . . . . . . . . . . . . . . . . . . . . . . . . . . .|16|



2 

|**6**|**Conclusion**|||**17**|
|---|---|---|---|---|
||6.1<br>Summary|.|. . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . .|17|
||6.2<br>Limitations||and Future Work . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . .|17|
|**7**|**References**|||**18**|



3 

## **1 Introduction** 

## **1.1 Background of the Source Academy** 

The Source Academy is a computer-mediated learning environment for studying the structure and interpretation of computer programs. Students write and run their programs in their web browser, using sublanguages of JavaScript called Source, designed for the textbook Structure and Interpretation of Computer Programs, JavaScript Edition. The Source Academy has in recent years gradually started to support more languages, such as Scheme, Python, C, and Java [2]. 

The Source Academy is used by numerous computer science courses at the National University of Singapore, among them CS1101S Programming Methodology, a course taken by over 1000 computer science first-year students every year. 

The Source Academy is a massive project, with not only a large number of users every year, but also with many contributors to the project. The repositories we are most interested in for this project have over 150 unique contributors [1]. 

Packages of additional functionality for the Source Academy are available; these are called Modules [3]. These make several functions available to user-written programs. For example, the Sound module allows user-written programs to convert functions representing a waveform to a sound file that can be listened to, and is used in teaching functional programming to first-year students [6]. 

Figure 1: The Source Academy user interface [2]. 

Figure 1 shows the Source Academy user interface. Note the code editor in the left half of the user interface, the REPL on the right half where outputs will be shown and additional code may be entered, 

4 

and the dropdown menu in the top right where you can select a language (here, “JavaScript” means the Source language). 

## **1.2 Definitions** 

As this project will interact with many different systems within the Source Academy and several concepts related to these systems, it is important to define terms relating to these concepts explicitly to reduce confusion in discussion. Several of these terms will appear in this report, and will be defined here. 

-  _User(-written) program_ : A program in a specific language, written by a Source Academy user in the editor. 

-  _Chunk_ : A piece of a user program, such as the content of the editor, or individual chunks entered in the REPL. 

-  _Language_ : A programming language and related data such as the editor’s syntax highlighting for this language, and associated with one or more evaluators. 

-  _Evaluator_ : A program that understands and can run user programs written in its language, and returns the result of running the user program or individual Chunks. 

-  _Entry point_ : A file where the evaluator should start its evaluation from. 

-  _Runner_ : The JavaScript thread running an Evaluator, data associated with this thread such as the status of the Evaluator, and methods to interact with the Evaluator (such as sending additional chunks to be run or receiving output from it). 

-  _Host_ : The JavaScript thread that created a given runner; each runner is tied to a host. In this project, we usually talk about the Source Academy frontend being the host, but the design is flexible and other hosts can be written. 

-  _Conductor_ : The name of the newly designed framework. Can also refer to its APIs. 

-  _Channel_ : A named communication link between the Host and a Runner. Each Channel carries one type of data, and, in general, each subsystem will have its own Channel. 

-  _Conduit_ : A collection of all the Channels between a particular Host and Runner. 

-  _Plugin_ : Additional pieces of functionality added on to the base Conductor functionality, usually involving additional type(s) of Host-Runner communication. Typically, each plugin should use its own Channel or set of Channels. 

## **1.3 Project Objectives** 

## **1.3.1 Supporting Implementation of New Languages** 

The primary objective of this project is to make the implementation of new languages easier. 

The Source Academy was initially designed to support one language, Source, and its sublanguages; today, the needs of the Source Academy has evolved beyond what it was initially designed to handle. Several languages have been forced on top of this design as a base, accruing a large amount of technical debt. Documentation on how to add a new language to the Source Academy is also lacking, and as a result this process of implementing a new language for the Source Academy requires a lot of knowledge of the inner workings of many systems and ends up feeling very obtuse. 

This project seeks to provide a simple interface that language implementers can utilise with minimal understanding of the inner workings of the Source Academy so as to better focus on the details of the new language rather than the details of the Source Academy. 

5 

## **1.3.2 Establishing a Common Interface to Interact with Modules** 

An extension of this primary objective is to create a way for Modules to support different languages – in particular, a way to transfer information between Modules and user programs, as different languages may have different ways of storing and handling data. For example, Source may simply store complex data as JavaScript objects, but a VM-based language like Java would need to be concerned about an underlying representation of this data in its memory system. 

This project seeks to establish a common interface for data transfer between different languages and Modules, in a way such that Modules do not need any knowledge of the language it is attached to in order to provide and receive data from it. 

## **1.3.3 Lazy Loading** 

A secondary objective is to support lazy loading of languages by the Source Academy. 

Currently, all of the available languages are loaded by the browser at once, and the desired language is then chosen from among them and used to run the given code. This is impractical if support for more languages were to be added, as the size of the file containing all the languages is likely to become extremely large. It is also wasteful as it is rare to require more than one language to be loaded at a time. 

This project seeks to allow languages to be loaded lazily by the Source Academy, such that only the required languages are loaded by the browser. 

## **1.3.4 Evaluating Programs using Web Workers** 

Another secondary objective is to move the evaluation of programs off the main JavaScript thread. 

At the moment, user-written programs are evaluated directly in the main JavaScript thread. This has the unfortunate effect of freezing the Source Academy user interface until the evaluation of the program is complete, as no other JavaScript can be run (for example, to update the user interface when buttons are clicked) while the evaluation of the program is in progress. 

This project seeks to move the evaluation of programs onto a separate JavaScript thread via Web Workers, preventing the evaluation of programs from making the user interface unresponsive. 

6 

## **2 Literature Review** 

## **2.1 Design Patterns** 

As this project involves architecting a new system with the goal of decoupling unrelated pieces of code, various design patterns were studied. In particular, the Observer pattern [5] and Event Queue pattern [9] was the inspiration of some design decisions made for the framework. 

## **2.1.1 Observer Pattern** 

The Observer pattern [5] is useful for decoupling components from each other. Suppose there are two components, a subscriber and a publisher. The subscriber (or observer) registers an event handler with the publisher; then, when the time is right, the publisher can go down the list of registered handlers and call each one without having any knowledge of who the subscriber is, decoupling the subscriber from the publisher. For example, the Source Academy frontend (publisher) does not need to directly tell the Source language Evaluator (subscriber) that the Run button was pressed to get it to start running the user program; instead, the Source language (or any other language, for that matter) can register itself with the frontend’s “run button clicked” event, and the frontend does not need to have any knowledge of which language is to be run in order to dispatch said event. In the framework’s design, I take this idea further by allowing any object to create events, thus also decoupling the publisher from the subscriber, and separating each event type into its own data stream; more on this in section 4.1. 

## **2.1.2 Event Queue Pattern** 

The Event Queue pattern [9] is also a useful decoupling pattern, and can be seen as an extension of the Observer pattern above. The setup is similar to the above, except that the subscriber only receives dispatched events when it is ready to process them. This is used in the processing of Chunks: the frontend can simply send new Chunks to the Evaluator’s queue without worrying if it is ready to process a new chunk (perhaps the Evaluator is still busy evaluating a previous Chunk). Then, when the Evaluator is ready to process a new Chunk, it can retrieve the next Chunk from the queue for processing. This idea is extended in the framework’s design using `Promise` s: a `Promise` is given to the Evaluator that will resolve to a new Chunk. If there are Chunks waiting in the queue, the resolution is immediate; otherwise, the Evaluator will wait for one to arrive before continuing. This greatly simplifies the design of the Chunk processing on the part of the language implementer. The Chunk processing is mentioned again in section 4.2. 

## **2.2 Usage of the Source Academy in Teaching** 

It was also important to keep in mind the ways in which the Source Academy is used, in order to prioritise the implementation of the most essential functionality. It is important to preserve the idea of sublanguages, as by restricting the capabilities of the language students use, the scaling of the course to learners with prior knowledge of programming is improved [4]. To that end, I designed languages to be able to have multiple Evaluators, so Source (for example) will be able to define different Evaluators for each sublanguage. 

7 

## **3 Pre-design Preparations** 

## **3.1 Project Planning** 

The stated objectives are pretty open-ended, so my initial few meetings with Prof. Henz was to narrow down how best to accomplish these objectives. Eventually, it was decided that the route I would be pursuing would be to architect a new framework to act as a middleman between the languages and the Source Academy itself. 

I would then meet with Prof. Henz on a weekly basis to discuss various ideas for the implementation of different parts of this new system. Through these meetings, these ideas would be refined before being implemented. 

I also discussed with many contributors of different languages supported (or soon-to-be-supported) by the Source Academy, in order to understand what they would need from such a system, and to hear any concerns they have about the implementation of the framework. 

## **3.2 Web Worker Proof of Concept** 

One of the secondary objectives was the moving of the evaluation of programs off the main JavaScript thread using Web Workers. There was a concern that doing so would cause a loss of functionality of important Modules being used in the instruction of courses like CS1101S, such as the Runes Module (used to draw pictures using predefined shapes), the Sounds Module (used to create sounds by defining waves), and the PixNFlix Module (used to create video filters). As it was deemed more important that these Modules will be able to be used over fulfilling this secondary objective, and the fact that deciding to use Web Workers or not would inform further design decisions to be made later on, it was decided that I will first produce a proof-of-concept to determine if these Modules’ functionalities will be affected by using Web Workers. 

Figure 2: A proof-of-concept showing that full functionality of Modules will be possible while using Web Workers. 

I produced a proof-of-concept, shown in Figure 2, demonstrating that the functionalities of these three Modules would be able to be implemented within Web Workers as well. It was shown to be possible to display Runes from within a Web Worker, produce a sound file from a Web Worker and play it in the 

8 

browser, and pass webcam video data to a Web Worker for it to apply a video filter on it, and display the output in the browser. 

## **3.3 Defining Terms** 

It was decided that it would help in the future design and discussions if certain concepts were given names and defined explicitly to avoid any confusion. I spent a few days considering the systems this project would interact with, and the associated concepts that needed defining. Several of these are used in this report and was given in section 1.2 above. 

Some of these terms had to be named carefully to reduce confusion as much as possible. For example, _Evaluator_ above was initially named _Interpreter_ , as the act of running a given string as a program within the JavaScript environment could be considered interpretation. However, some language implementations may compile the user program into an intermediate form before interpreting the result. The separate compiling and interpreting steps would commonly be labelled compilation and interpretation, and the corresponding parts of the program labelled compiler and interpreter. To avoid a potential conflict of terms, especially for implementers of these languages, the language implementation was renamed _Evaluator_ – thus, these implementers can now safely and unambiguously say that their evaluator is composed of a compiler and interpreter. Several other terms similarly had to be chosen carefully. 

9 

## **4 Architecting a new framework** 

## **4.1 Basic Design Ideas** 

-  Web Workers are created by passing the URL of the worker to the `Worker` constructor [8]. This means that all the logic necessary for running a language’s Evaluator should be contained within one JavaScript file. This is accomplished by using a bundler to combine multiple source files into one output JavaScript file. It would be most convenient for Evaluator implementers if the framework was designed as a library to be bundled together with the Evaluator’s logic. 

-  The main JavaScript thread can communicate with Web Workers via the `postMessage` function, which takes a JavaScript object. This object can be retrieved from the `message` event handler on the other thread [8]. As many different types of data will have to be transmitted between the threads, many unrelated to each other, it would be easier to manage if the messages were separated into their own data stream, with each stream handling only one type of messages. The `MessageChannel` API [7] allows the creation of separate data streams, each with their own `postMessage` and `message` event handlers. 

-  The designed framework should be powerful, allowing language implementers to easily customise behaviour as required, but at the same time ergonomic, being easy to use should customisation not be required. 

## **4.2 The First Attempt** 

Before even beginning to conceptualise a basic API (Application Programming Interface), it was necessary to know what key functionalities must be supported by it. At its core, the whole project is about evaluating the result of a given program. Thus, it was decided that the following must be supported must be supported by the basic API: 

-  Running a given string as a user program in a specific language (to run the user program that is in the editor) 

-  Running strings as Chunks in the same language (for REPL support) 

-  An output stream (for calls to `display` in the user program or other forms of standard-output, commonly used to view the values of variables or observe the number of times a function is called) 

-  An error stream (for errors while evaluating the program) 

-  A result stream (for the result of evaluating the program that is in the editor, or an expression that is from the REPL) 

With these in mind, the first API was developed. At this moment, it was considered more important to design the API well than to integrate it into the Source Academy interfaces and Evaluators; trying to integrate this new system directly into the Source Academy would use too much unnecessary effort, especially in this design phase where many things will change very quickly. Therefore, a basic user interface and Evaluator that uses the API was also developed. Some implementation details of this first attempt are as follows: 

-  A `MessageChannel` was used for Chunks, and another for all IO streams (standard output, error, and result streams) 

-  The entry point was simply considered just another Chunk, and sent on the Chunk MessageChannel after initialisation of the runner was complete 

-  Some experimental support for an input stream was added to the IO Channel 

-  Support for using `Promise` s to wait on inputs on both the Chunk MessageChannel and IO MessageChannel was added 

10 

-  A basic HTML page with two textareas (one to act as the editor input, and the other to act as REPL input) was created for the user interface 

-  A basic Evaluator that simply calls JavaScript’s `eval` on the given Chunks was used as the Evaluator 

## **4.3 The Second Attempt** 

It was noted that simply treating the entry point as a Chunk may not work for some languages like Java, where the name of the files are important. Furthermore, there was no support for reading of additional files, which is needed if pieces of programs are stored in separate files. 

Additionally, having to implement the same `Promise` -based waiting on messages for multiple channels led to the idea of abstracting the `message` event handler away in favour of `Promise` s. 

Thus, a second version of the API was developed, with the following changes from the first attempt: 

-  An abstraction on top of `MessageChannel` , `Channel` , was created to abstract away the `message` event handler 

-  All `MessageChannel` s were replaced by `Channel` s 

-  A new `Channel` was added for file support: the runner can send a file name on this channel, and the host would respond with the content of the file on the same channel 

-  The entry point was no longer sent as a Chunk after initialisation of the runner; instead, the name of the entry point file was sent in a separate message after initialisation 

## **4.4 The Third Attempt** 

It was observed that there would potentially be many `Channel` s created for different purposes when full functionality is implemented, especially when modules are supported (since it would make the most sense for each module to have its own `Channel` ), and that manually creating each `Channel` was not ergonomic. 

It was also observed that many languages do not care about files or their names at all, and having to implement a special case for the entry point when it could just be treated as another Chunk was not convenient for language implementers. 

Therefore, a third version was designed, with the following additional changes: 

-  A `Channel` handler, `Conduit` , was created; messages can be sent on specific channels by specifying the channel’s name 

-  An abstraction over the `Conduit` API, `BasicEvaluator` , was created with the intention of having evaluators extend from it; the default behaviour was to treat the entry point as a Chunk, but could be overriden if necessary 

## **4.5 Module Support** 

In order for Modules to be language-agnostic, a common interface would need to be designed, as mentioned in section 1.3.2. A list of data types common to all Modules was identified, and an API for Modules to read and write data of these types from the Evaluator. However, the migration of an existing Module to the Conductor framework is quite a lot of work, so a separate temporary API was designed with the goal of being very easy to migrate existing Modules to, and enable them to function with the Source language. This enables the use of all modules by Source, necessary for instruction in the coming semester, while allowing a loose schedule for the migration of these Modules to the new (permanent) API. The temporary API is described in more detail in Section 4.6.3. 

In addition, additional changes were made to the Source Academy frontend to support Modules. Modules are composed of functionality (functions that can be called from user code), and a visual 

11 

component, known as a Tab. As Modules are now running in the Runner environment, they are not able to communicate with their tabs using the previous mechanism (React context), as this lives in the Host environment. Instead, Module functionality (under both the permanent and temporary APIs) is now made into a Plugin running on the Runner; the module’s Tab is made into a Plugin running on the Host, and they may communicate using a Channel dedicated to that Module. 

## **4.6 The Final Design** 

## **4.6.1 Overview** 

Figure 3: The design of the Conductor framework, shown running with the Source Academy Frontend. One Module is attached, using the permanent Module API. 

The final design of the Conductor framework is shown in Figure 3. The key idea of providing multiple layers of abstraction within the API was what allowed the goal of customisability and developer ergonomics at the same time to be achieved. Customisation can be done at every level of abstraction, from the Evaluator’s behaviour at the highest level to the data sent through each Channel at the lowest level. For example, the default behaviour of treating files as chunks allow languages such as JavaScript and Python to simply implement a single function to evaluate a chunk for full functionality; however, it is extremely easy to override this behaviour to implement languages such as Java where the names of files might be important. All the components are also designed to be highly modular. The Plugin system in particular allows specific pieces of functionality to be attached to the Evaluator as required. For example, a Plugin for some visualisation can be included for one particular Evaluator, where it may communicate on its dedicated Channel without affecting the functionality of any other component. This Plugin will also not be included in any Evaluator does not use it, resulting in a smaller file size when built. 

12 

## **4.6.2 Flexibility of the Conductor Framework** 

Figure 4: The same Evaluator and Module, now running with a command line as a Host. 

Furthermore, the Conductor framework is designed to be very flexible. In Figure 4, the same Evaluator and Module from Figure 3 is now running with a command line as a Host instead of the Source Academy Frontend. There is no Tab or other compatible Plugin available for this host for this module, yet due to the decoupling of the Module functionality from the Source Academy Frontend, this will still be able to run. Any messages sent from Module A through its Channel will simply be lost, which does not impact the functioning of the Module – all its exported functions continue to be available to the Evaluator. An exception is if the Plugin on the Host side is expected to provide some input or response to the Module on the Runner side; in this case, the lack of a corresponding Plugin on the other end of the Channel means that any functionality depending on this input or response will wait indefinitely for these communications. 

## **4.6.3 The Temporary Module API** 

Figure 5: A different Module attached to the Source language via the temporary API. 

13 

Finally, as described in Section 4.5, migrating existing Modules to the Conductor framework is a time-consuming process. In order to ensure all existing Modules are able to be used for instruction in the upcoming semester, a temporary API was designed with the express purpose of being very easy to migrate to. Figure 5 shows such a Module that has been migrated to the temporary API. The functionality of the Module links to the Source Evaluator using the original mechanism that allows Source to import external Modules. The only change that needs to be made is that communication between the Module’s functionality and its Tab now goes through a Channel, instead of via React context. This change is minimal and allows all existing modules to be migrated quickly. 

## **4.7 Framework Trials** 

The newly developed Conductor framework was introduced in the course CS4215 Programming Language Implementation. In this course, students write an evaluator for a programming language as part of the coursework. In past iterations of this course, many students faced difficulty integrating their projects into the Source Academy system, owing to the great complexity of the Source Academy Frontend and its deep coupling with the Source language – many of these students instead opt to create their own interfaces to demonstrate their evaluators instead of integrating into the Source Academy. For the current iteration, to assist with the usage of the framework for the course’s term project as well as being a good starting point for migration of existing languages to the new framework, an example repository was created. It contains a barebones Evaluator to demonstrate functionality along with configuration files required to build and deploy the Evaluator. In the current iteration, students were able to use the Conductor framework to write an Evaluator for Rust and other languages that works directly with the deployed version of the Source Academy frontend with no changes required to any of the frontend code. This shows that the framework is reasonably easy to use, and more importantly, that the primary goal of allowing implementers of Evaluators to focus on the details of the Evaluator rather than fuss about integration with the Source Academy has been achieved. 

14 

## **5 Integration into the Source Academy** 

The Source Academy frontend is a large and complex codebase, and is tightly coupled to the Source language as it was originally built to run Source. Integrating Conductor directly into the frontend would change a large amount of the codebase at once, resulting in a large and complex pull request, and remove support for languages that have not been ported to the new framework (as Conductor would replace the original execution logic). 

Figure 6: The implemented feature flag system. The new framework has been enabled and will replace the original execution logic when the “Run” button is clicked. 

To facilitate incremental merging of code, a feature flag system was designed and introduced to the Source Academy frontend. Figure 6 shows the implemented feature flag system. This would allow features to be merged quickly, even if somewhat incomplete, as the new behaviour can be hidden behind a feature flag, with the default value only changed to enable the new feature once it has been fully tested. This reduces the probability and scale of merge conflicts, especially to features or other pull requests that involve large scale changes to the codebase - these may safely be broken down into multiple smaller pull requests without impact to regular users of the deployed frontend. 

With the feature flag system in place, the new execution method using the framework was implemented and hidden behind a feature flag. The feature flag allowed it to be merged despite not being fully ready to replace the existing execution method, especially since only a Source Evaluator is currently available for the new framework. A second feature flag allows the path of the evaluator to be modified, allowing new evaluators to be trialled directly on the deployed frontend instead of spinning up a local version for testing (a process which takes a few minutes). 

## **5.1 Languages Directory** 

Under Conductor, languages (and their Evaluators) are no longer bundled together into the Source Academy frontend, and are instead compiled and bundled separately. However, the Source Academy frontend still needs to know which languages are supported, and more importantly, where to find them. As the Source Academy frontend is just one of many potential Hosts, such a list of languages does not belong to it, and thus it was decided that a separate repository would be created to maintain a list of supported languages. 

Each language is described by its name, some information about the language, like syntax highlighting information, and an array of Evaluators that can run the language. A language may have multiple Evaluators as each Evaluator may have different capabilities. For example, one of the Evaluators may support a suite of visualisation tools, while another may drop support for these tools in exchange for being faster to execute. 

This repository contains an array of such language descriptions, which may be imported into each Host to have the list of languages available to each without making any Host have a dependency upon any other Host. 

15 

## **5.2 Migrating the Source Language onto the API** 

As the Source language is used for instruction, it was migrated onto Conductor as a demonstration of its capabilities and to prepare for its use in the upcoming semester. It was decided that it was simpler to adopt a “wrapper” style for the migration, as the original Source evaluator is large and complex, and it would be extremely complicated to combine all these parts into an Evaluator class, as required by Conductor. Instead, an Evaluator was written that acts like a wrapper around Conductor – it replaces the Source Academy Frontend in calling Source’s evaluation functions, and acts like the Source Academy Frontend when receiving evaluation outputs that it then redirects to the Conductor framework. 

## **5.3 Migrating Visualisation Tools** 

It was noted that the current visualisation tools, such as the CSE Machine and Data Visualiser, look and act very similarly to a Module Tab. This led to the realisation that these tools can be implemented as Conductor plugins. The two tools mentioned were converted to Host Plugins, each with their own Tab. A Runner Plugin was written for the CSE Machine visualisation, which is meant to be imported by any Evaluator supporting the visualisation and bundled together at build time – it cannot be used as a Module, as it does not expose any functionality to user programs. The Data Visualiser, on the other hand, was converted into a regular Module, to be imported by user programs at run time, since the way to use this visualisation was to call a `draw_data` function from user code, which could easily be a Module export instead of a built-in function. 

## **5.4 Plugins Directory** 

Under the Conductor framework, Plugins are additional pieces of functionality that may be added at build-time or run-time. It is desired that Plugins be built separately from other components like any Evaluators, the Source Academy Frontend, or Conductor itself. 

Currently, Source Academy’s Modules are built separately and hosted from a central location, and required Modules are loaded at run-time. Module resolution is achieved by appending the module’s name to a base URL. However, there are three problems with copying this approach. 

Firstly, not all Plugins may be Modules. It is important to distinguish non-Module Plugins and Modules, as the former does not use the Module API and attempting to import one as a Module would not work – it is desirable to be able to fail gracefully if such an attempt is made. 

Secondly, the resolution is not very flexible, as it may be desirable to host Plugins from somewhere other than a central location – for example, instead of forcing all Plugins into a single repository as is currently the case with Modules, each Plugin may exist in a separate repository. 

Finally, there may be multiple Plugins with the same name, intended to run in different environments. For example, there may be a Sound Module, which is a Plugin for Runners; then there would be a Sound Plugin for the Source Academy Frontend, containing the Module Tab needed to display a user interface to interact with the Module, such as play/pause controls; there may also be a Sound Plugin for a command line environment, containing some ability to write played sounds to disk instead. 

Drawing inspiration from the Languages Directory described in Section 5.1, a Plugins Directory repository was created to maintain a list of supported Plugins. 

Plugins are placed into logical groups, which is described by its name. Each group contains a mapping from environment to Plugin location: if a Runner wishes to import the “Sound” Module, it would look in the “Sound” Plugin group and look up the mapping for the Module environment; if the Source Academy Frontend wishes to import the same, it would look in the “Sound” Plugin group and look up the mapping for the Web environment. 

The repository contains an array of such Plugin groups, which may be imported by Runners and Hosts to have the list available to all of them without any dependency relation forming between them as a result. 

16 

## **6 Conclusion** 

## **6.1 Summary** 

The new Conductor framework has been deployed to the Source Academy frontend and is functional. Using the newly-developed feature flag system, it remains hidden behind a feature flag as feature-parity with the existing evaluation system has not been achieved (other non-Source languages have not been migrated yet and are thus unsupported). The implementation can be found in the Source Academy’s frontend repository. The Conductor framework itself can be found in the Source Academy’s Conductor repository. In addition, an example repository was created showcasing the usage of the framework, along with the necessary configuration to build and deploy the Evaluator. The Conductor framework and example repository has been trialled in the course CS4215 Programming Language Implementation with great success, enabling students to implement evaluators for Rust and other languages without making any changes in the frontend repository. 

A Languages Directory was created to be a central list of languages and their corresponding Evaluators. This enables any Host using the Conductor framework to run an Evaluator for any language on the list. In the same vein, a Plugins Directory was created to be a central list of Plugins, enabling any Host or Runner to lookup Plugins suitable for its use. 

The Source language was migrated to the new Conductor framework using a “wrapper” technique, with an Evaluator written that acts like the Source Academy Frontend that the Source language was coupled to. This Evaluator calls the Source language’s evaluation functions like the Source Academy Frontend would, and receives the evaluation outputs like the Source Academy Frontend before redirecting them to the framework. 

A Module was migrated to the new Conductor framework, using the Runner’s Module API. Migration of the other Modules to the temporary Module API is planned; all of these Modules will be listed in the Plugins Directory once the migration is complete. Some visualisation tools, such as the CSE Machine, were migrated to the new Conductor framework as Plugins. The Data Visualiser tool was migrated as a Module instead. 

## **6.2 Limitations and Future Work** 

Currently, almost all modules are or will be on the temporary API, which means they are not officially supported to run on non-Source languages by the framework (language implementations which pretend to be Source under the hood may still be able to use these modules). Work is required to migrate each module to the permanent API to enable their use by all languages. 

Furthermore, custom syntax highlighting is not currently supported. A discussion with the Source Academy team as well as members working on other Source Academy projects concluded that the differences in syntax highlighting information requirements between the frontend’s Ace editor and an upcoming Visual Studio Code extension means a more in-depth look will be needed to support both of these platforms. Currently, syntax highlighting information is defined with a generic type-parameter in anticipation of future work in this area. 

Finally, the “wrapper” approach taken with porting the Source language to the Conductor framework is more of a temporary measure to ensure availability of the Source Evaluator as soon as possible, as it is essential for instruction in the coming semester. Future work in this area could integrate the framework more deeply, removing the overhead of the wrapper and giving a cleaner and more easily maintainable codebase. 

17 

## **7 References** 

- [1] Source Academy. Github - source-academy/frontend: Frontend of source academy, an online experiential environment for computational thinking (react, redux, saga, blueprint). URL: `https: //github.com/source-academy/frontend` . 

- [2] Source Academy. Source academy. URL: `https://sourceacademy.org/playground` . 

- [3] Source Academy. Source academy modules. URL: `https://source-academy.github.io/modules/ documentation/index.html` . 

- [4] Boyd Anderson, Martin Henz, and Kok-Lim Low. Community-driven course and tool development for cs1. In _SIGCSE TS 2023: Proceedings of the 2023 ACM SIGCSE Technical Symposium on Computer Science Education, Toronto, Canada_ , mar 2023. URL: `https://dl.acm.org/doi/10. 1145/3545945.3569740` . 

- [5] Erich Gamma, John Vlissides, Ralph Johnson, and Richard Helm. _Design Patterns: Elements of Reusable Object-Oriented Software_ . Addison-Wesley, 1994. 

- [6] Martin Henz, Shang-Hui Koh, and Samyukta Sounderraman. Teachable moments in functional audio processing. In _Proceedings of the 2021 ACM SIGPLAN International Symposium on SPLASHE (SPLASH-E 2021), ACM, New York, NY, United States_ , pages 65–70, oct 2021. URL: `https: //doi.org/10.1145/3484272.3484967` . 

- [7] Mozilla. Messagechannel - web apis — mdn. URL: `https://developer.mozilla.org/en-US/docs/ Web/API/MessageChannel` . 

- [8] Mozilla. Web workers api - web apis — mdn. URL: `https://developer.mozilla.org/en-US/docs/ Web/API/Web_Workers_API` . 

- [9] Robert Nystrom. _Event Queue_ · _Decoupling Patterns_ · _Game Programming Patterns_ . 2014. URL: `https://gameprogrammingpatterns.com/event-queue.html` . 

18 

