// This file is part of the KDE libraries
// Copyright (C) 2008 Paul Giannaros <paul@giannaros.org>
// Copyright (C) 2009, 2010 Dominik Haumann <dhaumann kde org>
// Copyright (C) 2010 Joseph Wenninger <jowenn@kde.org>
//
// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Library General Public
// License as published by the Free Software Foundation; either
// version 2 of the License, or (at your option) version 3.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Library General Public License for more details.
//
// You should have received a copy of the GNU Library General Public License
// along with this library; see the file COPYING.LIB.  If not, write to
// the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
// Boston, MA 02110-1301, USA.

#include "katescript.h"
#include "katescriptdocument.h"
#include "katescriptview.h"
#include "katescripthelpers.h"
#include "kateview.h"
#include "katedocument.h"
#include "katepartdebug.h"

#include <iostream>
#include <KLocalizedString>

#include <QFile>
#include <QFileInfo>
#include <QScriptEngine>
#include <QScriptValue>
#include <QScriptContext>
#include <QMap>

//BEGIN conversion functions for Cursors and Ranges
/** Converstion function from KTextEditor::Cursor to QtScript cursor */
static QScriptValue cursorToScriptValue(QScriptEngine *engine, const KTextEditor::Cursor &cursor)
{
    QString code = QString::fromLatin1("new Cursor(%1, %2);").arg(cursor.line())
                   .arg(cursor.column());
    return engine->evaluate(code);
}

/** Converstion function from QtScript cursor to KTextEditor::Cursor */
static void cursorFromScriptValue(const QScriptValue &obj, KTextEditor::Cursor &cursor)
{
    cursor.setPosition(obj.property(QLatin1String("line")).toInt32(),
                       obj.property(QLatin1String("column")).toInt32());
}

/** Converstion function from QtScript range to KTextEditor::Range */
static QScriptValue rangeToScriptValue(QScriptEngine *engine, const KTextEditor::Range &range)
{
    QString code = QString::fromLatin1("new Range(%1, %2, %3, %4);").arg(range.start().line())
                   .arg(range.start().column())
                   .arg(range.end().line())
                   .arg(range.end().column());
    return engine->evaluate(code);
}

/** Converstion function from QtScript range to KTextEditor::Range */
static void rangeFromScriptValue(const QScriptValue &obj, KTextEditor::Range &range)
{
    range.setRange(KTextEditor::Cursor(obj.property(QLatin1String("start")).property(QLatin1String("line")).toInt32(),
                                       obj.property(QLatin1String("start")).property(QLatin1String("column")).toInt32()),
                   KTextEditor::Cursor(obj.property(QLatin1String("end")).property(QLatin1String("line")).toInt32(),
                                       obj.property(QLatin1String("end")).property(QLatin1String("column")).toInt32()));
}
//END

KateScript::KateScript(const QString &urlOrScript, enum InputType inputType)
    : m_loaded(false)
    , m_loadSuccessful(false)
    , m_url(inputType == InputURL ? urlOrScript : QString())
    , m_engine(0)
    , m_document(0)
    , m_view(0)
    , m_inputType(inputType)
    , m_script(inputType == InputSCRIPT ? urlOrScript : QString())
{
}

KateScript::~KateScript()
{
    if (m_loadSuccessful) {
        // remove data...
        delete m_engine;
        delete m_document;
        delete m_view;
    }
}

QString KateScript::backtrace(const QScriptValue &error, const QString &header)
{
    QString bt;
    if (!header.isNull()) {
        bt += header + QLatin1String(":\n");
    }
    if (error.isError()) {
        bt += error.toString() + QLatin1Char('\n');
    }

    bt += m_engine->uncaughtExceptionBacktrace().join(QLatin1String("\n")) + QLatin1Char('\n');

    return bt;
}

void KateScript::displayBacktrace(const QScriptValue &error, const QString &header)
{
    if (!m_engine) {
        std::cerr << "KateScript::displayBacktrace: no engine, cannot display error\n";
        return;
    }
    std::cerr << "\033[31m" << qPrintable(backtrace(error, header)) << "\033[0m" << '\n';
}

void KateScript::clearExceptions()
{
    if (!load()) {
        return;
    }
    m_engine->clearExceptions();
}

QScriptValue KateScript::global(const QString &name)
{
    // load the script if necessary
    if (!load()) {
        return QScriptValue();
    }
    return m_engine->globalObject().property(name);
}

QScriptValue KateScript::function(const QString &name)
{
    QScriptValue value = global(name);
    if (!value.isFunction()) {
        return QScriptValue();
    }
    return value;
}

bool KateScript::load()
{
    if (m_loaded) {
        return m_loadSuccessful;
    }

    m_loaded = true;
    m_loadSuccessful = false; // here set to false, and at end of function to true

    // read the script file into memory
    QString source;
    if (m_inputType == InputURL) {
        if (!Kate::Script::readFile(m_url, source)) {
            return false;
        }
    } else {
        source = m_script;
    }

    // create script engine, register meta types
    m_engine = new QScriptEngine();
    qScriptRegisterMetaType(m_engine, cursorToScriptValue, cursorFromScriptValue);
    qScriptRegisterMetaType(m_engine, rangeToScriptValue, rangeFromScriptValue);

    // export read & require function and add the require guard object
    m_engine->globalObject().setProperty(QLatin1String("read"), m_engine->newFunction(Kate::Script::read));
    m_engine->globalObject().setProperty(QLatin1String("require"), m_engine->newFunction(Kate::Script::require));
    m_engine->globalObject().setProperty(QLatin1String("require_guard"), m_engine->newObject());

    // export debug function
    m_engine->globalObject().setProperty(QLatin1String("debug"), m_engine->newFunction(Kate::Script::debug));

    // export translation functions
    m_engine->globalObject().setProperty(QLatin1String("i18n"), m_engine->newFunction(Kate::Script::i18n));
    m_engine->globalObject().setProperty(QLatin1String("i18nc"), m_engine->newFunction(Kate::Script::i18nc));
    m_engine->globalObject().setProperty(QLatin1String("i18ncp"), m_engine->newFunction(Kate::Script::i18ncp));
    m_engine->globalObject().setProperty(QLatin1String("i18np"), m_engine->newFunction(Kate::Script::i18np));

    // register default styles as ds* global properties
    m_engine->globalObject().setProperty(QLatin1String("dsNormal"), KTextEditor::dsNormal);
    m_engine->globalObject().setProperty(QLatin1String("dsKeyword"), KTextEditor::dsKeyword);
    m_engine->globalObject().setProperty(QLatin1String("dsFunction"), KTextEditor::dsFunction);
    m_engine->globalObject().setProperty(QLatin1String("dsVariable"), KTextEditor::dsVariable);
    m_engine->globalObject().setProperty(QLatin1String("dsControlFlow"), KTextEditor::dsControlFlow);
    m_engine->globalObject().setProperty(QLatin1String("dsOperator"), KTextEditor::dsOperator);
    m_engine->globalObject().setProperty(QLatin1String("dsBuiltIn"), KTextEditor::dsBuiltIn);
    m_engine->globalObject().setProperty(QLatin1String("dsExtension"), KTextEditor::dsExtension);
    m_engine->globalObject().setProperty(QLatin1String("dsPreprocessor"), KTextEditor::dsPreprocessor);
    m_engine->globalObject().setProperty(QLatin1String("dsAttribute"), KTextEditor::dsAttribute);
    m_engine->globalObject().setProperty(QLatin1String("dsChar"), KTextEditor::dsChar);
    m_engine->globalObject().setProperty(QLatin1String("dsSpecialChar"), KTextEditor::dsSpecialChar);
    m_engine->globalObject().setProperty(QLatin1String("dsString"), KTextEditor::dsString);
    m_engine->globalObject().setProperty(QLatin1String("dsVerbatimString"), KTextEditor::dsVerbatimString);
    m_engine->globalObject().setProperty(QLatin1String("dsSpecialString"), KTextEditor::dsSpecialString);
    m_engine->globalObject().setProperty(QLatin1String("dsImport"), KTextEditor::dsImport);
    m_engine->globalObject().setProperty(QLatin1String("dsDataType"), KTextEditor::dsDataType);
    m_engine->globalObject().setProperty(QLatin1String("dsDecVal"), KTextEditor::dsDecVal);
    m_engine->globalObject().setProperty(QLatin1String("dsBaseN"), KTextEditor::dsBaseN);
    m_engine->globalObject().setProperty(QLatin1String("dsFloat"), KTextEditor::dsFloat);
    m_engine->globalObject().setProperty(QLatin1String("dsConstant"), KTextEditor::dsConstant);
    m_engine->globalObject().setProperty(QLatin1String("dsComment"), KTextEditor::dsComment);
    m_engine->globalObject().setProperty(QLatin1String("dsDocumentation"), KTextEditor::dsDocumentation);
    m_engine->globalObject().setProperty(QLatin1String("dsAnnotation"), KTextEditor::dsAnnotation);
    m_engine->globalObject().setProperty(QLatin1String("dsCommentVar"), KTextEditor::dsCommentVar);
    m_engine->globalObject().setProperty(QLatin1String("dsRegionMarker"), KTextEditor::dsRegionMarker);
    m_engine->globalObject().setProperty(QLatin1String("dsInformation"), KTextEditor::dsInformation);
    m_engine->globalObject().setProperty(QLatin1String("dsWarning"), KTextEditor::dsWarning);
    m_engine->globalObject().setProperty(QLatin1String("dsAlert"), KTextEditor::dsAlert);
    m_engine->globalObject().setProperty(QLatin1String("dsOthers"), KTextEditor::dsOthers);
    m_engine->globalObject().setProperty(QLatin1String("dsError"), KTextEditor::dsError);

    // register scripts itself
    QScriptValue result = m_engine->evaluate(source, m_url);
    if (hasException(result, m_url)) {
        return false;
    }

    // AFTER SCRIPT: set the view/document objects as necessary
    m_engine->globalObject().setProperty(QLatin1String("document"), m_engine->newQObject(m_document = new KateScriptDocument()));
    m_engine->globalObject().setProperty(QLatin1String("view"), m_engine->newQObject(m_view = new KateScriptView()));

    // yip yip!
    m_loadSuccessful = true;

    return true;
}

QScriptValue KateScript::evaluate(const QString& program, const FieldMap& env)
{
    if ( !load() ) {
        qWarning() << "load of script failed:" << program;
        return QScriptValue();
    }

    // set up stuff in a new context, to not pollute the global stuff
    auto context = m_engine->pushContext();

    auto obj = context->activationObject();
    for ( auto it = env.begin(); it != env.end(); it++ ) {
        obj.setProperty(it.key(), *it);
    }
    auto result = m_engine->evaluate(program);

    m_engine->popContext();
    return result;
}

bool KateScript::hasException(const QScriptValue &object, const QString &file)
{
    if (m_engine->hasUncaughtException()) {
        displayBacktrace(object, i18n("Error loading script %1\n", file));
        m_errorMessage = i18n("Error loading script %1", file);
        delete m_engine;
        m_engine = 0;
        m_loadSuccessful = false;
        return true;
    }
    return false;
}

bool KateScript::setView(KTextEditor::ViewPrivate *view)
{
    if (!load()) {
        return false;
    }
    // setup the stuff
    m_document->setDocument(view->doc());
    m_view->setView(view);
    return true;
}

void KateScript::setGeneralHeader(const KateScriptHeader &generalHeader)
{
    m_generalHeader = generalHeader;
}

KateScriptHeader &KateScript::generalHeader()
{
    return m_generalHeader;
}

